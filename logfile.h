// Arquivo de log do co-op (cryoffear\Mod coop\cofmp_log.txt), escrito pelo cofmp.dll ("jogo") e pelo tunel ("tunel").
// Cada linha e um WriteFile em modo append, entao os dois processos podem escrever ao mesmo tempo.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>

// Pasta do mod dentro de cryoffear: log, crash dumps, backup do liblist.gam e o tunel (subpasta tunel).
// Ficam fora dela so o que o jogo precisa achar no lugar dele: cl_dlls\cofmp.dll, o liblist.gam e o simon.mdl.
#define COFMP_DIR "Mod coop"
#define COFMP_TUNNEL_DIR COFMP_DIR "\\tunel"

static char g_logPath[MAX_PATH];

// dir: pasta Mod coop (sem barra no fim); cria se nao existir
static void LogFile_Open(const char* dir)
{
	CreateDirectoryA(dir, NULL);
	_snprintf(g_logPath, sizeof(g_logPath), "%s\\cofmp_log.txt", dir);
	g_logPath[sizeof(g_logPath) - 1] = 0;
	// passou de 5 MB: guarda o anterior e comeca outro
	WIN32_FILE_ATTRIBUTE_DATA fa;
	if (GetFileAttributesExA(g_logPath, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 5u << 20))
	{
		char prev[MAX_PATH];
		_snprintf(prev, sizeof(prev), "%s\\cofmp_log_anterior.txt", dir);
		prev[sizeof(prev) - 1] = 0;
		MoveFileExA(g_logPath, prev, MOVEFILE_REPLACE_EXISTING);
	}
}

static void LogFile_Write(const char* tag, const char* text)
{
	if (!g_logPath[0])
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	char line[1100];
	int n = _snprintf(line, sizeof(line) - 2, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] %s", t.wYear, t.wMonth, t.wDay,
		t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, tag, text);
	if (n < 0)
		n = sizeof(line) - 2;
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		n--;
	line[n++] = '\r';
	line[n++] = '\n';
	HANDLE f = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	DWORD w;
	WriteFile(f, line, n, &w, NULL);
	CloseHandle(f);
}
