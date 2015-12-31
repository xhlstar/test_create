/** 测试 speex 的 aec
	实际测试发现，win32的 waveOutxxx 回调精度太差，20ms 时，但是 waveInxxx 看起来，跟理论值相符，
	不知道这个是个体现象，还是普遍现象，因此 waveOutOpen() 时，不再指定callback，而是在
	waveOutProc() 中同时处理 waveInAddBuffer()+waveOutWrite()

	@date 2011-5-16: 使用一个环形缓冲，保存 waveOut 数据
		回调周期使用 80ms，这时 waveInxxx, waveOutxxx 似乎正常

	@date 2011-5-17: 采用另外的方式：
				waveInProc() 把声音保存到 _cbuf_input,
				waveOutProc() 把声音保存到 _cbuf_output,
				独立的高优先级线程，对 _cbuf_input, _cbuf_output 进行处理

				wavexxx 效率实在不行，开始使用dsound试试吧
 */

#define USING_DSOUND 1
#define SAVEFILE 1
#define USING_DSOUND_FULLDUPLEX 0

#include <stdio.h>
#include <stdlib.h>
#include <winsock2.h>
#include <mmsystem.h>
#include <assert.h>
#include <deque>
#include <process.h>
#if USING_DSOUND 
#	include <dsound.h>
#	pragma comment(lib, "dsound.lib")
#	pragma comment(lib, "dxguid.lib")
#endif // 

#include "../../../include/speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#include "util_cirbuf.h"

#ifndef __func__
#	define __func__ __FUNCTION__
#endif // 

// 8k
#define SAMPLE_RATE 8000
// 20ms
#define FRAMESIZE (160)
#define BUFS 3

// 数据使用 40ms，非常希望使用 20ms，但是看起来windows无法达到这个精度
#define BUFSIZE (FRAMESIZE*2*2)

// 环形缓冲大小
#define CBUFSIZE 4096

// listen sock, udp, port 7777
static SOCKET _sock_listen = INVALID_SOCKET;
#define LISTEN_PORT 7777

// sender sock
static SOCKET _sock_sender = INVALID_SOCKET;
static HWAVEIN _wave_in = 0;
static HWAVEOUT _wave_out = 0;
const char *_target_ip;

#if USING_DSOUND
static LPDIRECTSOUNDCAPTURE8 _ds_capture = 0;
static LPDIRECTSOUND8 _ds_playback = 0;
static unsigned char *_ds_playback_buf = 0;	// 用于 player 的缓冲， BUFSIZE * BUFS
static LPDIRECTSOUNDBUFFER8 _ds_playback_buffer = 0;
static LPDIRECTSOUNDCAPTUREBUFFER8 _ds_capture_buffer = 0;
static DWORD _ds_capture_pos = 0, _ds_playback_pos = 0;	// Lock offset
#endif // 

// inproc, outproc 次数
static unsigned int _cnt_in = 0, _cnt_out = 0;

// speex 
static SpeexEchoState *_speex_aec = 0;
static SpeexPreprocessState *_speex_preprocess = 0;

static tea_cirbuf_t *_cbuf_input, *_cbuf_output;	// 环形缓冲，用于同步 speak, mic
static tea_cirbuf_t *_cbuf_recv;	// 网络接收缓冲
static char _silence_data[BUFSIZE];	// 静音数据

// 两个事件对象，对应着 waveInProc, waveOutProc
static HANDLE _evt_notify[2] = { CreateEvent(0, 0, 0, 0), CreateEvent(0, 0, 0, 0), };

static void send_pcm (void *p, int len);

// 从 cbuf 中提取数据
static void popdata (tea_cirbuf_t *cbuf, void *buf, size_t len)
{
	assert(util_cbuf_data(cbuf) >= len);
	void *pdata;
	size_t csize = util_cbuf_get_cdata(cbuf, &pdata);
	if (csize >= len) {
		memcpy(buf, pdata, len);
		util_cbuf_consume(cbuf, len);
	}
	else {
		memcpy(buf, pdata, csize);
		util_cbuf_consume(cbuf, csize);

		buf = (char*)buf + csize;
		len -= csize;
		csize = util_cbuf_get_cdata(cbuf, &pdata);
		memcpy(buf, pdata, len);
		util_cbuf_consume(cbuf, len);
	}
}

// thread of speex aec
static unsigned __stdcall thread_aec (void *param)
{
	HRESULT hr;
	timeBeginPeriod(1);
	static DWORD _t_begin = timeGetTime(), _t_last = timeGetTime();
	static size_t _cnt_total = 0, _cnt_last = 0;

#if SAVEFILE
#endif // save pcm file

	// 用于合成
	short buf_in[FRAMESIZE], buf_out[FRAMESIZE], outbuf[FRAMESIZE];

	DWORD pos_write = 0;

	while (1) {
#if USING_DSOUND
		DWORD rc = WaitForMultipleObjects(2, _evt_notify, FALSE, -1);
		if (rc == WAIT_OBJECT_0) {
			_cnt_in++;

			// position
			DWORD wp, rp;
			hr = _ds_capture_buffer->GetCurrentPosition(&wp, &rp);

			// capture notify
			void *p1, *p2;
			DWORD l1, l2;
			hr = _ds_capture_buffer->Lock(rp, BUFSIZE, &p1, &l1, &p2, &l2, 0);
			if (hr != S_OK) {
				fprintf(stderr, "%s: capture Lock err\n", __func__);
				exit(-1);
			}
			
			// save
			if (util_cbuf_space(_cbuf_input) < BUFSIZE) {
				fprintf(stderr, "\n%s: capture buf overflow!!!!\n", __func__);
				// exit(-1);
				//util_cbuf_consume(_cbuf_input, CBUFSIZE/2);
				util_cbuf_consume(_cbuf_input, util_cbuf_data(_cbuf_input)); // 清空得了
			}
			util_cbuf_save(_cbuf_input, p1, l1);
			util_cbuf_save(_cbuf_input, p2, l2);

			_ds_capture_buffer->Unlock(p1, l1, p2, l2);
		}
		else {
			_cnt_out++;

			// playback notify
			DWORD wp, pp;
			hr = _ds_playback_buffer->GetCurrentPosition(&pp, &wp);

			void *p1, *p2;
			DWORD l1, l2;
			hr = _ds_playback_buffer->Lock(wp, BUFSIZE, &p1, &l1, &p2, &l2, 0);
			//hr = _ds_playback_buffer->Lock(pos_write, BUFSIZE, &p1, &l1, &p2, &l2, 0);
			if (hr != S_OK) {
				fprintf(stderr, "\n%s: playback Lock err\n", __func__);
				::exit(-1);
			}

			pos_write += BUFSIZE;
			pos_write %= (BUFSIZE*BUFS);

			// get from _cbuf_recv
			char buf[BUFSIZE];
			char *p = _silence_data;
			if (util_cbuf_data(_cbuf_recv) >= BUFSIZE) {
				popdata(_cbuf_recv, buf, BUFSIZE);
				p = buf;
//				printf(".");
			}
			else {
				printf("S");
			}

			if (l1 >= BUFSIZE) {
				memcpy(p1, p, l1);
			}
			else {
				memcpy(p1, p, l1);
				memcpy(p2, p+l1, l2);
			}

			hr = _ds_playback_buffer->Unlock(p1, l1, p2, l2);

			// save to _cbuf_output
			if (util_cbuf_space(_cbuf_output) < BUFSIZE) {
				fprintf(stderr, "\n%s: playback buf overflow!\n", __func__);
//				exit(-1);
				// util_cbuf_consume(_cbuf_output, CBUFSIZE/2);
				util_cbuf_consume(_cbuf_output, util_cbuf_data(_cbuf_output));	// 清空得了
			}
			util_cbuf_save(_cbuf_output, p, BUFSIZE);
		}
		
#else
		// 等待in，out都完成
		WaitForMultipleObjects(2, _evt_notify, TRUE, -1);
		
#endif // 
		_cnt_total++;
		_cnt_last++;
/**
		DWORD t_curr = timeGetTime();
		if (t_curr - _t_last > 1000) {
			fprintf(stderr, "tr=%.5f, lr=%.5f, cid=%u, cod=%u, in=%u, out=%d\t\t\r", 
				(double)(t_curr - _t_begin)/_cnt_total, 
				(double)(t_curr - _t_last)/_cnt_last,
				util_cbuf_data(_cbuf_input),
				util_cbuf_data(_cbuf_output),
				_cnt_in, _cnt_out);
			_cnt_last = 0;
			_t_last = t_curr;
		}		
*/
		while (util_cbuf_data(_cbuf_input) >= sizeof(buf_in) && 
			util_cbuf_data(_cbuf_output) >= sizeof(buf_out)) {

			popdata(_cbuf_input, buf_in, sizeof(buf_in));
			popdata(_cbuf_output, buf_out, sizeof(buf_out));

			// speex_echo_cancel
			speex_echo_cancellation(_speex_aec, buf_in, buf_out, outbuf);
			speex_preprocess_run(_speex_preprocess, outbuf);

			// 发送 outbuf
			send_pcm(outbuf, sizeof(outbuf));

#if SAVEFILE
			FILE *fp_capture = fopen("capture.pcm", "ab");
			FILE *fp_playback = fopen("playback.pcm", "ab");
			FILE *fp_aec = fopen("aecd.pcm", "ab");

			fwrite(buf_in, 1, sizeof(buf_in), fp_capture);
			fwrite(buf_out, 1, sizeof(buf_out), fp_playback);
			fwrite(outbuf, 1, sizeof(outbuf), fp_aec);

			fclose(fp_capture);
			fclose(fp_playback);
			fclose(fp_aec);
#endif // 
		}
	}

	return 1;
}

// thread of recv
static unsigned __stdcall thread_recv(void *param)
{
	while (1) {
		char buf[65536];
		sockaddr_in from;
		int len = sizeof(from);
		int rc = recvfrom(_sock_listen, buf, sizeof(buf), 0, (sockaddr*)&from, &len);
		if (rc > 0) {
			if (util_cbuf_space(_cbuf_recv) < (size_t)rc) {
				fprintf(stderr, "%s: _cbuf_recv OVERFLOW!!!!!\n", __func__);
			}
			else {
				util_cbuf_save(_cbuf_recv, buf, rc);
			}
		}
		else {
			fprintf(stderr, "%s: recvfrom err\n", __FUNCTION__);
			exit(-1);
		}
	}

	return 0;
}

static void send_pcm (void *p, int len)
{
	sockaddr_in to;
	to.sin_family = AF_INET;
	to.sin_port = htons(LISTEN_PORT);
	to.sin_addr.s_addr = inet_addr(_target_ip);
	int x = sendto(_sock_sender, (char*)p, len, 0, (sockaddr*)&to, sizeof(to));
	if (x != len) {
		fprintf(stderr, "%s: err\n", __FUNCTION__);
		exit(-1);
	}
}

// open listener
static int open_listener()
{
	_sock_listen = socket(AF_INET, SOCK_DGRAM, 0);
	if (_sock_listen == INVALID_SOCKET) {
		fprintf(stderr, "%s: err\n", __FUNCTION__);
		return -1;
	}

	sockaddr_in sin;
	sin.sin_family = AF_INET;
	sin.sin_port = htons(LISTEN_PORT);
	sin.sin_addr.s_addr = INADDR_ANY;
	if (bind(_sock_listen, (sockaddr*)&sin, sizeof(sin)) < 0) {
		fprintf(stderr, "%s: bind %d err\n", __FUNCTION__, LISTEN_PORT);
		closesocket(_sock_listen);
		_sock_listen = INVALID_SOCKET;
		return -1;
	}

	// 启动接收线程
	_beginthreadex(0, 0, thread_recv, 0, 0, 0);

	return 1;
}

// open sender
static int open_sender(const char *ip)
{
	_sock_sender = socket(AF_INET, SOCK_DGRAM, 0);
	_target_ip = ip;
	return 1;	
}

struct WaveBuffer
{
	WAVEHDR s_hdr[BUFS];
	char s_buf[BUFSIZE][BUFS];
};

static WaveBuffer _buffer_mic, _buffer_speaker;
static bool _sync = false;

// callback of mic
static void CALLBACK mic_callback (HWAVEIN hwi, UINT msg, 
								   DWORD_PTR ins, DWORD_PTR p1, DWORD_PTR p2)
{
	if (msg == WIM_OPEN) {
	}
	else if (msg == WIM_DATA) {
		WAVEHDR *hdr = (WAVEHDR*)p1;

		if (_sync) {
			// 保存到 _cbuf_input
			if (util_cbuf_space(_cbuf_input) < hdr->dwBytesRecorded) {
				fprintf(stderr, "%s: recording OVERFLOW!!!!!\n", __FUNCTION__);
				exit(-1);
			}

			util_cbuf_save(_cbuf_input, hdr->lpData, hdr->dwBytesRecorded);
			SetEvent(_evt_notify[0]);
		}

		hdr->dwFlags &= ~WHDR_DONE;
		hdr->dwBytesRecorded = 0;
		waveInAddBuffer(hwi, hdr, sizeof(WAVEHDR));

		_cnt_in ++;
	}
}

// callback of speaker
static void CALLBACK speaker_callback (HWAVEOUT hwo, UINT msg,
									   DWORD_PTR ins, DWORD_PTR p1, DWORD_PTR p2)
{
	char buf[BUFSIZE];

	if (msg == WOM_OPEN) {
	}
	else if (msg == WOM_DONE) {
		WAVEHDR *hdr = (WAVEHDR*)p1;

		// 从 _cbuf_recv 中提取
		void *p = _silence_data;
		if (util_cbuf_data(_cbuf_recv) >= BUFSIZE) {
			popdata(_cbuf_recv, buf, BUFSIZE);
			p = buf;
//			printf("D");
		}
		else {
			printf("S");
		}

		memcpy(hdr->lpData, p, BUFSIZE);

		hdr->dwFlags &= ~WHDR_DONE;
		waveOutWrite(hwo, hdr, sizeof(WAVEHDR));

		// 保存到 _cbuf_output
		if (util_cbuf_space(_cbuf_output) < BUFSIZE) {
			fprintf(stderr, "%s: playback OVERFLOW!!!\n", __func__);
			exit(-1);
		}

		util_cbuf_save(_cbuf_output, p, BUFSIZE);

		_sync = true;

		SetEvent(_evt_notify[1]);

		_cnt_out++;
	}
}

// open mic, 8k 16bit, mono
static int open_mic()
{
	WAVEFORMATEX wfx;
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = 1;
	wfx.nSamplesPerSec = SAMPLE_RATE;
	wfx.wBitsPerSample = 16;
	wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
	wfx.cbSize = 0;

#if USING_DSOUND
	if (S_OK != DirectSoundCaptureCreate8(0, &_ds_capture, 0)) {
		fprintf(stderr, "%s: DirectSoundCaptureCreate8 err\n", __func__);
		exit(-1);
	}

	DSCBUFFERDESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.dwSize = sizeof(desc);
	desc.dwFlags = 0;
	desc.dwBufferBytes = BUFS*BUFSIZE;
	desc.lpwfxFormat = &wfx;
	LPDIRECTSOUNDCAPTUREBUFFER buffer;
	if (S_OK != _ds_capture->CreateCaptureBuffer(&desc, &buffer, 0)) {
		fprintf(stderr, "%s: CreateCaptureBuffer err\n", __func__);
		exit(-1);
	}
	buffer->QueryInterface(IID_IDirectSoundCaptureBuffer8, (void**)&_ds_capture_buffer);
	buffer->Release();

	LPDIRECTSOUNDNOTIFY8 notify;
	_ds_capture_buffer->QueryInterface(IID_IDirectSoundNotify8, (void**)&notify);
	DSBPOSITIONNOTIFY pts[BUFS];
	for (int i = 0; i < BUFS; i++) {
		pts[i].dwOffset = (i+1) * BUFSIZE - 1;
		pts[i].hEventNotify = _evt_notify[0];
	}
	notify->SetNotificationPositions(BUFS, pts);
	notify->Release();

	_ds_capture_buffer->Start(DSCBSTART_LOOPING);

#else
	waveInOpen(&_wave_in, WAVE_MAPPER, &wfx, (DWORD_PTR)mic_callback, 0, CALLBACK_FUNCTION | WAVE_FORMAT_DIRECT);

	// prepare mic buf，每个缓冲 80ms，最多
	for (int i = 0; i < BUFS; i++) {
		_buffer_mic.s_hdr[i].lpData = _buffer_mic.s_buf[i];
		_buffer_mic.s_hdr[i].dwBufferLength = BUFSIZE;
		_buffer_mic.s_hdr[i].dwFlags = 0;
		int err = waveInPrepareHeader(_wave_in, &_buffer_mic.s_hdr[i], sizeof(WAVEHDR));
	}

	for (int i = 0; i < BUFS; i++) {
		int err = waveInAddBuffer(_wave_in, &_buffer_mic.s_hdr[i], sizeof(WAVEHDR));
	}

	waveInStart(_wave_in);
#endif // dsound
	
	return 1;
}

static int open_speaker_mic()
{
	WAVEFORMATEX wfx;
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = 1;
	wfx.nSamplesPerSec = SAMPLE_RATE;
	wfx.wBitsPerSample = 16;
	wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
	wfx.cbSize = 0;

	DSBUFFERDESC p_desc;
	memset(&p_desc, 0, sizeof(p_desc));
	p_desc.dwSize = sizeof(p_desc);
	p_desc.dwFlags = DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GETCURRENTPOSITION2 |
		DSBCAPS_GLOBALFOCUS | DSBCAPS_LOCSOFTWARE;	// notify
	p_desc.dwBufferBytes = BUFSIZE*BUFS;		// 
	p_desc.lpwfxFormat = &wfx;

	DSCBUFFERDESC c_desc;
	memset(&c_desc, 0, sizeof(c_desc));
	c_desc.dwSize = sizeof(c_desc);
	c_desc.dwFlags = 0;
	c_desc.dwBufferBytes = BUFS*BUFSIZE;
	c_desc.lpwfxFormat = &wfx;

	IDirectSoundFullDuplex8 *dsfd;
	HRESULT hr = DirectSoundFullDuplexCreate8(0, 0,
		&c_desc, &p_desc, GetDesktopWindow(),
		DSSCL_EXCLUSIVE,
		&dsfd,
		&_ds_capture_buffer,
		&_ds_playback_buffer,
		0);

	LPDIRECTSOUNDNOTIFY8 notify;
	_ds_capture_buffer->QueryInterface(IID_IDirectSoundNotify8, (void**)&notify);
	DSBPOSITIONNOTIFY pts[BUFS];
	for (int i = 0; i < BUFS; i++) {
		pts[i].dwOffset = (i+1) * BUFSIZE - 1;
		pts[i].hEventNotify = _evt_notify[0];
	}
	notify->SetNotificationPositions(BUFS, pts);
	notify->Release();

	_ds_playback_buffer->QueryInterface(IID_IDirectSoundNotify8, (void**)&notify);
	for (int i = 0; i < BUFS; i++) {
		pts[i].dwOffset = (i+1) * BUFSIZE - 1;
		pts[i].hEventNotify = _evt_notify[1];
	}
	notify->SetNotificationPositions(BUFS, pts);
	notify->Release();

	// 首先预先填充 playback buf?

	hr = _ds_capture_buffer->Start(DSCBSTART_LOOPING);
	hr = _ds_playback_buffer->Play(0, 0, DSBPLAY_LOOPING);

	return 1;
}

// open speaker
static int open_speaker()
{
	WAVEFORMATEX wfx;
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = 1;
	wfx.nSamplesPerSec = SAMPLE_RATE;
	wfx.wBitsPerSample = 16;
	wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
	wfx.cbSize = 0;

#if USING_DSOUND
	if (S_OK != DirectSoundCreate8(0, &_ds_playback, 0)) {
		fprintf(stderr, "%s: DirectSoundCreate8 err\n", __func__);
		exit(-1);
	}

	if (S_OK != _ds_playback->SetCooperativeLevel(GetDesktopWindow(), DSSCL_EXCLUSIVE)) {
		fprintf(stderr, "%s: SetCooperativeLevel err\n", __func__);
		exit(-1);
	}

	//  DSBUFFERDESC 
	DSBUFFERDESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.dwSize = sizeof(desc);
	desc.dwFlags = DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GETCURRENTPOSITION2 |
		DSBCAPS_LOCSOFTWARE | DSBCAPS_GLOBALFOCUS;	// notify
	desc.lpwfxFormat = &wfx;
	desc.dwBufferBytes = BUFSIZE*BUFS;		// 

	LPDIRECTSOUNDBUFFER buffer;

	if (S_OK != _ds_playback->CreateSoundBuffer(&desc, &buffer, 0)) {
		fprintf(stderr, "%s: CreateSoundBuffer err\n", __func__);
		exit(-1);
	}

	buffer->QueryInterface(IID_IDirectSoundBuffer8, (void**)&_ds_playback_buffer);
	buffer->Release();

	// buffer
	_ds_playback_buf = (unsigned char*)malloc(BUFS * BUFSIZE);

	// notify points
	LPDIRECTSOUNDNOTIFY8 notify;
	_ds_playback_buffer->QueryInterface(IID_IDirectSoundNotify8, (void**)&notify);
	DSBPOSITIONNOTIFY pts[BUFS];
	for (int i = 0; i < BUFS; i++) {
		pts[i].dwOffset = (i+1) * BUFSIZE - 1;
		pts[i].hEventNotify = _evt_notify[1];
	}
	notify->SetNotificationPositions(BUFS, pts);
	notify->Release();

	// start buffer
	if (S_OK != _ds_playback_buffer->Play(0, 0, DSBPLAY_LOOPING)) {
		fprintf(stderr, "%s: Play err\n", __func__);
		exit(-1);
	}

#else
	waveOutOpen(&_wave_out, WAVE_MAPPER, &wfx, 
		(DWORD_PTR)speaker_callback, 0, CALLBACK_FUNCTION | WAVE_FORMAT_DIRECT);

	// prepare out buf
	for (int i = 0; i < BUFS; i++) {
		_buffer_speaker.s_hdr[i].lpData = _buffer_speaker.s_buf[i];
		_buffer_speaker.s_hdr[i].dwBufferLength = BUFSIZE;
		_buffer_speaker.s_hdr[i].dwFlags = 0;
		int err = waveOutPrepareHeader(_wave_out, &_buffer_speaker.s_hdr[i], sizeof(WAVEHDR));
	}

	waveOutPause(_wave_out);

	for (int i = 0; i < BUFS; i++) {
		memcpy(&_buffer_speaker.s_buf[i], _silence_data, BUFSIZE);
		waveOutWrite(_wave_out, &_buffer_speaker.s_hdr[i], sizeof(WAVEHDR));
	}

	waveOutRestart(_wave_out);
#endif

	return 1;
}

int main (int argc, char **argv)
{
	int rc;
	if (argc != 2) {
		fprintf(stderr, "usage: %s <target ip>\n", argv[0]);
		exit(-1);
	}
	
#if SAVEFILE
	DeleteFileA("capture.pcm");
	DeleteFileA("playback.pcm");
	DeleteFileA("aecd.pcm");
#endif // 

	WSADATA data;
	WSAStartup(0x202, &data);

	// 静音
	memset(_silence_data, 0, BUFSIZE);

	// 环形缓冲
	_cbuf_recv = util_cbuf_create(64*1024);
	_cbuf_output = util_cbuf_create(CBUFSIZE);
	_cbuf_input = util_cbuf_create(CBUFSIZE);


	// 启动speex工作线程
	HANDLE th = (HANDLE)_beginthreadex(0, 0, thread_aec, 0, 0, 0);
	SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);

	// open speex aec, 20ms, 250ms
	int sampleRate = 8000;
	_speex_aec = speex_echo_state_init(FRAMESIZE, 2000);
	_speex_preprocess = speex_preprocess_state_init(FRAMESIZE, sampleRate);
	speex_echo_ctl(_speex_aec, SPEEX_ECHO_SET_SAMPLING_RATE, &sampleRate);
	speex_preprocess_ctl(_speex_preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, _speex_aec);

	// open listener
	rc = open_listener();

	// open sender
	rc = open_sender(argv[1]);

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

#if USING_DSOUND_FULLDUPLEX
	rc = open_speaker_mic();
#else
	// open speaker
	rc = open_speaker();

	// open mic
	rc = open_mic();
#endif // 
	while (1) {
		fprintf(stdout, "press 'q' to quit....\n");
		if (getchar() == 'q') {
			break;
		}
	}

	return 0;
}
