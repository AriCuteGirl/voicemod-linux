# Builds the Wine-side pieces with mingw-w64 (x86_64-w64-mingw32-gcc). The import
# libraries and DDK headers come with mingw-w64 itself.
CC = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall -fno-builtin
LDFLAGS = -shared -nostdlib
# mingw's ntddk.h includes <wdm.h>, so its ddk directory must be on the include path.
INCDIRS := $(shell $(CC) -xc -E -v /dev/null 2>&1 | sed -n '/<...> search starts here:/,/End of search list/s/^ //p')
DDK ?= $(firstword $(patsubst %/wdm.h,%,$(wildcard $(addsuffix /ddk/wdm.h,$(INCDIRS)))))

all: shim/version.dll driver/vmstub.sys

shim/version.dll: shim/version.c shim/sessions.c shim/crt.c shim/sessions.h shim/version.def
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--entry,DllMain -o $@ shim/version.c shim/sessions.c shim/crt.c shim/version.def \
		-lkernel32 -lole32 -luser32

driver/vmstub.sys: driver/vmstub.c
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--subsystem,native -Wl,--entry,DriverEntry -I$(DDK) -o $@ $< -lntoskrnl

clean:
	rm -f shim/version.dll driver/vmstub.sys

.PHONY: all clean
