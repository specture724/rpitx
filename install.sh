#!/bin/sh

echo "Install rpitx - some packages need internet connection -"

ARCH="$(uname -m)"

# The dependencies (csdr, librpitx, ft8_lib) are fetched as git submodules.
# Clone the repository with --recursive, or run "git submodule update --init".
require_submodule() {
  dir="$1"
  # .git is a directory for standalone checkouts but a gitdir pointer file
  # for submodules populated by "git clone --recursive".
  if [ ! -e "$dir/.git" ]; then
    echo "ERROR: dependency '$dir' is missing." >&2
    echo "Please clone this repository with --recursive, or run:" >&2
    echo "  git submodule update --init" >&2
    exit 1
  fi
}

# Warn when a submodule is not at the revision recorded in the gitlink
# (e.g. after a plain clone that never ran "git submodule update").
check_submodule_revision() {
  dir="$1"
  RECORDED="$(git ls-files -s -- "$dir" 2>/dev/null | awk '$1 == 160000 {print $2}')"
  CURRENT="$(git -C "$dir" rev-parse HEAD 2>/dev/null)"
  if [ -n "$RECORDED" ] && [ -n "$CURRENT" ] && [ "$CURRENT" != "$RECORDED" ]; then
    echo "Warning: $dir is at $CURRENT but the recorded revision is $RECORDED"
    echo "  run: git submodule update --init (or checkout $RECORDED)"
  fi
}

sudo apt-get update
sudo apt-get install -y libsndfile1-dev git
sudo apt-get install -y imagemagick libfftw3-dev raspi-utils
# libraspberrypi-dev is only packaged on 32-bit Raspberry Pi OS. On 64-bit OS
# (Pi 4/5) the VideoCore userland was removed and librpitx no longer needs it,
# so a missing package is not an error.
sudo apt-get install -y libraspberrypi-dev || echo "Note: libraspberrypi-dev not available on this OS - not needed"
#For rtl-sdr use
sudo apt-get install -y rtl-sdr buffer

# We use CSDR as a dsp for analogs modes thanks to HA7ILM
require_submodule csdr
check_submodule_revision csdr
cd csdr || exit
make && sudo make install
cd ../ || exit

cd src || exit
require_submodule librpitx
check_submodule_revision librpitx
cd librpitx/src || exit
# Modern Raspberry Pi OS (Bookworm+) and all 64-bit OS no longer ship the
# VideoCore userland under /opt/vc (including libbcm_host). librpitx provides
# its own bcm_host helpers, so strip these obsolete flags that break the build.
sed -i -e 's|-L/opt/vc/lib||g' -e 's|-lbcm_host||g' Makefile
make && sudo make install
cd ../../ || exit

# The Makefile still references sources that were never committed (pissb) or
# with wrong paths (piam, pidcf77, pifm), which makes "make install" fail.
# Patch it so install builds and installs what it lists.
sed -i \
  -e 's|\.\./fm/pifm\.c|fm/pifm.c|g' \
  -e 's|\.\./am/piam\.c|am/piam.c|g' \
  -e 's|\.\./dcf77/pidcf77\.c|dcf77/pidcf77.c|g' \
  -e 's|^install: all$|install: all ../piam ../pidcf77|' \
  -e '/install -m 0755 \.\.\/pissb/d' \
  Makefile

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
  echo "64-bit OS detected: DVB-T (dvbrf) uses 32-bit ARM assembler, skipping it."
  sed -i -e '/^all:/s| \.\./dvbrf||' -e '/install -m 0755 \.\.\/dvbrf/d' Makefile
fi

cd pift8 || exit
require_submodule ft8_lib
check_submodule_revision ft8_lib
cd ft8_lib || exit
make && sudo make install
cd ../ || exit
make
cd ../ || exit

make && sudo make install
cd .. || exit

# Pi 5: build the PIO-based transmitter tools (official /dev/pio0 API).
# These only make sense on the Raspberry Pi 5 (RP1 PIO hardware).
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
  if (cd src && make ../piofm ../pio_fsk \
      && sudo install -m 0755 ../piofm /usr/local/bin/piofm \
      && sudo install -m 0755 ../pio_fsk /usr/local/bin/pio_fsk); then
    echo "PIO transmitter tools installed (piofm, pio_fsk)"
  else
    echo "Warning: PIO transmitter tools could not be built (kernel headers missing?)"
  fi
fi

printf "\n\n"
printf "In order to run properly, rpitx need to modify /boot/config.txt. Are you sure (y/n) "
read -r CONT

if [ "$CONT" = "y" ]; then
  echo "Set GPU to 250Mhz in order to be stable"
   LINE='gpu_freq=250'
   if [ ! -f /boot/firmware/config.txt ]; then
   echo "Raspbian 11 or below detected using /boot/config.txt"
   FILE='/boot/config.txt'
   else
   echo "Raspbian 12 detected using /boot/firmware/config.txt"
   FILE='/boot/firmware/config.txt'
   fi
   grep -qF "$LINE" "$FILE"  || echo "$LINE" | sudo tee --append "$FILE"
   #PI4
   LINE='force_turbo=1'
   grep -qF "$LINE" "$FILE"  || echo "$LINE" | sudo tee --append "$FILE"
   echo "Installation completed !"
else
  echo "Warning : Rpitx should be instable and stop from transmitting !";
fi
