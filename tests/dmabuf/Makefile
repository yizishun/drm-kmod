PACKAGE=	tests
WARNS?=		1

TESTSDIR=       ${TESTSBASE}/sys/dmabuf

ATF_TESTS_C+=	udmabuf
CFLAGS+=	-I${.CURDIR:H:H}/drivers/dma-buf

LDFLAGS+=	-lprivateatf-c

.include <bsd.test.mk>
