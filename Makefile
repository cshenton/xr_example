LIBLINK := -Ldeps/lib -lopenxr_loader -lSDL2 -lSDL2main
WINLINK := -lkernel32 -ldinput8 -ldxguid -ladvapi32 -lsetupapi -lwinmm -lpathcch -nostdlib -lucrt
WINLINK += -limm32 -lole32 -loleaut32 -lversion -luuid -luser32 -lshell32 -lgdi32 -lopengl32
# -Wl,-subsystem:windows
game:
	clang -o game.exe src/*.c deps/src/*.c -Ideps/include -Iinclude -O2 $(LIBLINK) $(WINLINK)

run:
	./game.exe
