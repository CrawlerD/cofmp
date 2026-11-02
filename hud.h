// hud.h - janela do co-op dentro do jogo (tecla Home). Ver hud.cpp.
#pragma once

typedef void (*HudCmd_t)(const char* cmd); // manda texto para o console do engine (thread principal)

void Hud_Start(HudCmd_t cmd); // uma vez, no GiveFnptrsToDll
void Hud_RunPending();        // roda os comandos que a janela enfileirou (thread principal)
