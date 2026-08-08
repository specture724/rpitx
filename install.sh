#!/bin/sh

echo "Install rpitx - some packages need internet connection -"

ARCH="$(uname -m)"

# Resolve the dependency forks from this repository's origin, so that a clone
# of your own rpitx fork also pulls your matching forks (e.g. specture724/csdr,
# specture724/librpitx, specture724/ft8_lib). Defaults to the upstream owner.
GITHUB_USER="F5OEO"
ORIGIN_URL="$(git remote get-url origin 2>/dev/null)"
case "$ORIGIN_URL" in
  *github.com:*/*) GITHUB_USER="$(echo "$ORIGIN_URL" | sed -n 's#.*github.com[:/]\([^/]*\)/.*#\1#p')" ;;
  *github.com/*/*) GITHUB_USER="$(echo "$ORIGIN_URL" | sed -n 's#.*github.com/\([^/]*\)/.*#\1#p')" ;;
esac
[ -n "$GITHUB_USER" ] || GITHUB_USER="F5OEO"
echo "Using dependency forks from github.com/$GITHUB_USER"

# Clone a repo if not already present, retrying on transient network errors.
clone_repo() {
  repo="$1"
  dir="$2"
  if [ ! -d "$dir/.git" ]; then
    rmdir "$dir" 2>/dev/null || true
    attempt=0
    while [ "$attempt" -lt 5 ]; do
      attempt=$((attempt + 1))
      echo "Cloning $repo (attempt $attempt/5)..."
      # Force HTTP/1.1: GitHub's HTTP/2 can drop large clones with
      # "curl 92 ... CANCEL" errors, especially on slower connections.
      if git -c http.version=HTTP/1.1 clone "$repo" "$dir"; then
        return 0
      fi
      if [ "$attempt" -lt 5 ]; then
        echo "Clone failed, retrying in 5 seconds..."
        sleep 5
      fi
    done
    echo "Failed to clone $repo" >&2
    exit 1
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
clone_repo "https://github.com/$GITHUB_USER/csdr" csdr
cd csdr || exit
make && sudo make install
cd ../ || exit

cd src || exit
clone_repo "https://github.com/$GITHUB_USER/librpitx" librpitx
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
clone_repo "https://github.com/$GITHUB_USER/ft8_lib" ft8_lib
cd ft8_lib || exit
make && sudo make install
cd ../ || exit
make
cd ../ || exit

make && sudo make install
cd .. || exit

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
