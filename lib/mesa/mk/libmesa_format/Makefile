#	$OpenBSD: Makefile,v 1.1 2020/08/26 06:02:40 jsg Exp $

LIB=	mesa_format

NOPROFILE=

SRCS=	u_format.c \
	u_format_bptc.c \
	u_format_etc.c \
	u_format_latc.c \
	u_format_other.c \
	u_format_rgtc.c \
	u_format_s3tc.c \
	u_format_tests.c \
	u_format_yuv.c \
	u_format_zs.c

SRCS+=	u_format_table.c

.include "../Makefile.inc"

CFLAGS+=	${C_VIS_ARGS}
CPPFLAGS+=	-I${MESA_SRC}/src/util/format \
		-I${MESA_SRC}/src/gallium/include \
		-I${MESA_SRC}/src/gallium/auxiliary \
		-I${MESA_SRC}/src/mesa

install:

obj: _xenocara_obj

.include <bsd.lib.mk>
.include <bsd.xorg.mk>

.PATH:	${MESA_SRC}/src/util/format
.PATH:	${MESA_SRC}/generated/src/util/format
