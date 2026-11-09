APP_VERSION ?= 0.1.12

.PHONY: all app overlay dist clean

all: dist

app:
	$(MAKE) -C app APP_VERSION=$(APP_VERSION)

overlay:
	$(MAKE) -C overlay APP_VERSION=$(APP_VERSION)

dist: app overlay
	mkdir -p pack/switch/.overlays
	cp app/nexstatus.nro pack/switch/nexstatus.nro
	cp overlay/nexstatus.ovl pack/switch/.overlays/nexstatus.ovl
	if command -v python3 >/dev/null; then py=python3; else py="py -3"; fi; \
	$$py -c "import os,zipfile; z=zipfile.ZipFile('nexstatus.zip','w',compression=zipfile.ZIP_STORED); [z.write(os.path.join(d,n), os.path.relpath(os.path.join(d,n),'pack').replace(chr(92),'/')) for d,_,ns in os.walk('pack') for n in ns]; z.close()"

clean:
	$(MAKE) -C app clean
	$(MAKE) -C overlay clean
	rm -rf pack nexstatus.zip
