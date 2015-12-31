#ifndef _util_cirbuf__hh
#define _util_cirbuf__hh

/** 通用环形缓冲
 */
struct tea_cirbuf_t;

#ifdef __cplusplus
extern "C" {
#endif // c++

/** 创建一个环形缓冲，最大空间 size， size 必须为 2 的整数幂
 */
struct tea_cirbuf_t *util_cbuf_create (size_t size);

void util_cbuf_release (struct tea_cirbuf_t *buf);

/** 返回空间长度
 */
size_t util_cbuf_space (struct tea_cirbuf_t *buf);

/** 返回数据长度
 */
size_t util_cbuf_data (struct tea_cirbuf_t *buf);
size_t util_cbuf_contiguous_data (struct tea_cirbuf_t *buf);

/** 返回连续数据指针
 */
void *util_cbuf_get_contiguous_data (struct tea_cirbuf_t *buf);
size_t util_cbuf_get_cdata (struct tea_cirbuf_t *buf, void **data);

/** 添加数据, len 必须小于 util_cbuf_space()
 */
void util_cbuf_save (struct tea_cirbuf_t *buf, const void *data, size_t len);

/** 消耗数据长度
 */
void util_cbuf_consume (struct tea_cirbuf_t *buf, size_t cnt);

#ifdef __cplusplus
}
#endif // c++

#endif // util_cirbuf.h

