/* Camera.lua 硬编码 V4L2 ioctl 编号与结构尺寸的一致性自检（F-24 同族）
 * 全部为编译期断言：常量漂移（换内核/换架构）在构建期即失败，不再依赖人工比对。
 * 期望值出处：modules/Camera.lua 的 ffi 声明段与加载期 assert。
 * 用 _Static_assert 而非 #if：_IOW/_IOWR 宏展开含 sizeof，#if 表达式不支持。 */
#include <stdio.h>
#include <time.h>		/* v4l2_event 用到 struct timespec（严格 c11 下需显式包含） */
#include <sys/time.h>		/* v4l2_buffer 用到 struct timeval */
#include <linux/videodev2.h>

_Static_assert(VIDIOC_QUERYCAP == 0x80685600, "VIDIOC_QUERYCAP != 0x80685600");
_Static_assert(VIDIOC_S_FMT == 0xc0d05605, "VIDIOC_S_FMT != 0xc0d05605");
_Static_assert(VIDIOC_REQBUFS == 0xc0145608, "VIDIOC_REQBUFS != 0xc0145608");
_Static_assert(VIDIOC_QUERYBUF == 0xc0585609, "VIDIOC_QUERYBUF != 0xc0585609");
_Static_assert(VIDIOC_QBUF == 0xc058560f, "VIDIOC_QBUF != 0xc058560f");
_Static_assert(VIDIOC_DQBUF == 0xc0585611, "VIDIOC_DQBUF != 0xc0585611");
_Static_assert(VIDIOC_STREAMON == 0x40045612, "VIDIOC_STREAMON != 0x40045612");
_Static_assert(VIDIOC_STREAMOFF == 0x40045613, "VIDIOC_STREAMOFF != 0x40045613");

/* 结构尺寸是 _IOWR 里编码的那个长度：ffi 声明比内核小 → ioctl 时越界读写内存 */
_Static_assert(sizeof(struct v4l2_buffer) == 88, "sizeof(v4l2_buffer) != 88");
_Static_assert(sizeof(struct v4l2_format) == 208, "sizeof(v4l2_format) != 208");
_Static_assert(sizeof(struct v4l2_pix_format) == 48, "sizeof(v4l2_pix_format) != 48");
_Static_assert(sizeof(struct v4l2_capability) == 104, "sizeof(v4l2_capability) != 104");
_Static_assert(sizeof(struct v4l2_requestbuffers) == 20, "sizeof(v4l2_requestbuffers) != 20");
_Static_assert(sizeof(struct v4l2_timecode) == 16, "sizeof(v4l2_timecode) != 16");

_Static_assert(V4L2_BUF_TYPE_VIDEO_CAPTURE == 1, "V4L2_BUF_TYPE_VIDEO_CAPTURE != 1");
_Static_assert(V4L2_MEMORY_MMAP == 1, "V4L2_MEMORY_MMAP != 1");
_Static_assert(V4L2_CAP_VIDEO_CAPTURE == 0x00000001, "V4L2_CAP_VIDEO_CAPTURE != 1");
_Static_assert(V4L2_PIX_FMT_MJPEG == 0x47504a4d, "V4L2_PIX_FMT_MJPEG != 0x47504a4d");

int main(void) {
	printf("V4L2 常量自检通过：Device/Camera ioctl 编号与结构尺寸均与内核头文件一致\n");
	return 0;
}
