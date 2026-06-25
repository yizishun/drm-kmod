SRCDIR=	${.CURDIR:H:H}/drivers/dma-buf

.PATH:	${SRCDIR}

.include "../../kconfig.mk"
.include "../../linuxkpi_version.mk"

KMOD=	udmabuf
SRCS=	udmabuf.c

SRCS+=	device_if.h \
	bus_if.h \
	vnode_if.h

CLEANFILES+= ${KMOD}.ko.full ${KMOD}.ko.debug

CFLAGS+= -I${.CURDIR:H:H}/linuxkpi/gplv2/include
CFLAGS+= -I${.CURDIR:H:H}/linuxkpi/bsd/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/common/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/dummy/include

CFLAGS+= '-DKBUILD_MODNAME="${KMOD}"'
CFLAGS+= ${KCONFIG:C/(.*)/-DCONFIG_\1/}

CWARNFLAGS.udmabuf.c+=	-Wno-pointer-arith

.include <bsd.kmod.mk>
