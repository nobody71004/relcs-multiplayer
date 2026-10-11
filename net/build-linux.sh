#!/bin/bash
# net/build-linux.sh — build reLCS-server / reLCS-master / relcs-netbot on Linux.
# Usage: ./build-linux.sh          (outputs to net/bin/)
#        CC=clang CXX=clang++ ./build-linux.sh
set -euo pipefail
cd "$(dirname "$0")"
CC=${CC:-gcc}
CXX=${CXX:-g++}
mkdir -p obj bin

FLAGS="-O2 -DNOMINMAX -DENET_STATIC -D_GNU_SOURCE -I../src/net -I../vendor/enet/include -I../vendor/sqlite"
CXXFLAGS="$FLAGS -std=c++17"
# Link the C++ runtime statically: the build host's libstdc++ is often much
# newer than the deploy target's (a g++ 15 build needs GLIBCXX_3.4.34, Ubuntu
# 24.04 ships 3.4.32 -> "version `GLIBCXX_3.4.34' not found" at startup).
# libc/libpthread stay dynamic; only the C++ runtime is bundled.
LDFLAGS="-static-libstdc++ -static-libgcc"

echo "== enet =="
for f in ../vendor/enet/*.c; do
  case "$(basename "$f")" in
    win32.c) continue ;;
  esac
  $CC $FLAGS -c "$f" -o "obj/enet_$(basename "$f" .c).o"
done

echo "== sqlite =="
$CC $FLAGS -c ../vendor/sqlite/sqlite3.c -o obj/sqlite3.o

echo "== net-server =="
$CXX $CXXFLAGS -c server/server.cpp   -o obj/server.o
$CXX $CXXFLAGS -c server/gamemode.cpp -o obj/gamemode.o
$CXX $CXXFLAGS -c server/profiles.cpp -o obj/profiles.o
$CXX $CXXFLAGS -c server/resource.cpp -o obj/resource.o
$CXX $CXXFLAGS -c server/main.cpp     -o obj/server_main.o
$CXX -o bin/reLCS-server obj/server.o obj/gamemode.o obj/profiles.o obj/resource.o obj/server_main.o obj/enet_*.o obj/sqlite3.o -lpthread $LDFLAGS

echo "== net-master =="
$CXX $CXXFLAGS -c master/main.cpp      -o obj/master_main.o
$CXX $CXXFLAGS -c master/favorites.cpp -o obj/master_favorites.o
$CXX -o bin/reLCS-master obj/master_main.o obj/master_favorites.o obj/enet_*.o obj/sqlite3.o -lpthread $LDFLAGS

echo "== net-bot =="
$CXX $CXXFLAGS -c bot/main.cpp -o obj/bot_main.o
$CXX -o bin/relcs-netbot obj/bot_main.o obj/enet_*.o obj/sqlite3.o -lpthread $LDFLAGS

echo "== net-tests =="
# tests/main.cpp lives in tests/ but includes "profiles.h"/"resource.h" from
# server/ (the VS target has includedirs { "server" } for the same reason).
$CXX $CXXFLAGS -Iserver -c tests/main.cpp -o obj/tests_main.o
$CXX -o bin/relcs-net-tests obj/tests_main.o obj/profiles.o obj/resource.o obj/enet_*.o obj/sqlite3.o -lpthread $LDFLAGS

echo "built:"
ls -la bin/reLCS-server bin/reLCS-master bin/relcs-netbot bin/relcs-net-tests
