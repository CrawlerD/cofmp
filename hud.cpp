// hud.cpp - janela do co-op dentro do jogo (tecla Home): hospedar ou entrar num servidor pelo tunel da Steam.
//
// Roda numa thread propria do cofmp.dll no processo do jogo (o engine carrega o gamedll ja no menu).
// Os comandos do console (map, connect...) precisam rodar na thread principal: ficam numa fila que a
// janela do jogo (subclassificada) esvazia ao receber WM_HUD_CMD, e o StartFrame tambem esvazia.
// O tunel (CoF_Tunel.exe + steam_api.dll) e copiado pelo instalador para cl_dlls\cofmp_tunel.

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <algorithm>
#include "hud.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
	"version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")

static HudCmd_t g_cmd;
static CRITICAL_SECTION g_lock;
static std::string g_pending;
static HWND g_game, g_hud;
static WNDPROC g_gameProc;
static bool g_gameUnicode;
static HANDLE g_job, g_tunnel;
static std::string g_cry, g_tunnelDir;
static DWORD g_codeStart;
static HFONT g_font, g_bold;

enum { ID_PLAYERS = 100, ID_MAP, ID_HOST, ID_CODE, ID_COPY, ID_JOIN_CODE, ID_PASTE, ID_JOIN, ID_STATUS, ID_GO };
enum { WM_HUD_CMD = WM_APP + 0x43 };
enum { TIMER_POLL = 1, TIMER_CODE, TIMER_CONNECT };
static const int TUNNEL_PORT = 27020;

#ifndef COFMP_VERSION
#define COFMP_VERSION "?"
#endif

// ---------------------------------------------------------------- comandos na thread principal

void Hud_RunPending()
{
	if (!g_cmd)
		return;
	std::string s;
	EnterCriticalSection(&g_lock);
	s.swap(g_pending);
	LeaveCriticalSection(&g_lock);
	if (!s.empty())
		g_cmd(s.c_str());
}

static void Queue(const char* cmd)
{
	EnterCriticalSection(&g_lock);
	g_pending += cmd;
	LeaveCriticalSection(&g_lock);
	if (g_game)
		PostMessageW(g_game, WM_HUD_CMD, 0, 0);
}

static LRESULT CALLBACK GameProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == WM_HUD_CMD)
	{
		Hud_RunPending();
		return 0;
	}
	return g_gameUnicode ? CallWindowProcW(g_gameProc, h, m, w, l) : CallWindowProcA(g_gameProc, h, m, w, l);
}

static BOOL CALLBACK FindGameWnd(HWND h, LPARAM)
{
	DWORD pid;
	GetWindowThreadProcessId(h, &pid);
	char cls[64];
	if (pid != GetCurrentProcessId() || h == g_hud || !IsWindowVisible(h) || !GetClassNameA(h, cls, sizeof(cls)) ||
		!strcmp(cls, "ConsoleWindowClass"))
		return TRUE;
	RECT r;
	GetWindowRect(h, &r);
	if (r.right - r.left < 320 || r.bottom - r.top < 200)
		return TRUE;
	g_game = h;
	return FALSE;
}

static void AttachGame()
{
	if (g_game && IsWindow(g_game))
		return;
	g_game = NULL;
	EnumWindows(FindGameWnd, 0);
	if (!g_game)
		return;
	g_gameUnicode = IsWindowUnicode(g_game) != 0;
	g_gameProc = g_gameUnicode ? (WNDPROC)SetWindowLongPtrW(g_game, GWLP_WNDPROC, (LONG_PTR)GameProc)
							   : (WNDPROC)SetWindowLongPtrA(g_game, GWLP_WNDPROC, (LONG_PTR)GameProc);
	SetWindowLongPtrW(g_hud, GWLP_HWNDPARENT, (LONG_PTR)g_game); // fica sempre na frente do jogo
}

// ---------------------------------------------------------------- tunel

static void StopTunnel()
{
	if (!g_tunnel)
		return;
	TerminateProcess(g_tunnel, 0);
	CloseHandle(g_tunnel);
	g_tunnel = NULL;
}

static bool TunnelAlive()
{
	return g_tunnel && WaitForSingleObject(g_tunnel, 0) == WAIT_TIMEOUT;
}

static bool StartTunnel(const std::string& args)
{
	StopTunnel();
	std::string exe = g_tunnelDir + "\\CoF_Tunel.exe", cl = "\"" + exe + "\" " + args;
	STARTUPINFOA si = { sizeof(si) };
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_SHOWMINNOACTIVE; // fica na barra de tarefas sem tirar o foco do jogo
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(exe.c_str(), &cl[0], NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, g_tunnelDir.c_str(), &si, &pi))
		return false;
	if (g_job)
		AssignProcessToJobObject(g_job, pi.hProcess); // fecha junto com o jogo
	CloseHandle(pi.hThread);
	g_tunnel = pi.hProcess;
	return true;
}

// codigo do host = SteamID64 de conta individual (o tunel confere o mesmo)
static bool ValidCode(const std::string& s)
{
	return !s.empty() && s.size() <= 20 && s.find_first_not_of("0123456789") == std::string::npos &&
		(_strtoui64(s.c_str(), NULL, 10) >> 52) == 0x011;
}

// ---------------------------------------------------------------- janela

static void Status(const wchar_t* s)
{
	SetDlgItemTextW(g_hud, ID_STATUS, s);
}

static std::string ItemText(int id)
{
	char buf[128];
	GetDlgItemTextA(g_hud, id, buf, sizeof(buf));
	std::string s = buf;
	s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
		s.end());
	return s;
}

static std::string ClipboardText()
{
	std::string s;
	if (!OpenClipboard(g_hud))
		return s;
	HANDLE h = GetClipboardData(CF_TEXT);
	const char* p = h ? (const char*)GlobalLock(h) : NULL;
	if (p)
	{
		s.assign(p, strnlen(p, 128));
		GlobalUnlock(h);
	}
	CloseClipboard();
	s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
		s.end());
	return s;
}

static void CopyText(const std::string& s)
{
	if (!OpenClipboard(g_hud))
		return;
	EmptyClipboard();
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
	memcpy(GlobalLock(h), s.c_str(), s.size() + 1);
	GlobalUnlock(h);
	SetClipboardData(CF_TEXT, h);
	CloseClipboard();
}

static void ShowHud(bool show)
{
	if (!show)
	{
		ShowWindow(g_hud, SW_HIDE);
		if (g_game)
			SetForegroundWindow(g_game);
		return;
	}
	RECT r, w;
	GetWindowRect(g_hud, &w);
	if (!g_game || !GetWindowRect(g_game, &r))
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
	int x = (r.left + r.right - (w.right - w.left)) / 2, y = (r.top + r.bottom - (w.bottom - w.top)) / 2;
	SetWindowPos(g_hud, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
	SetForegroundWindow(g_hud);
	// codigo do amigo ja copiado: cola sozinho
	std::string clip = ClipboardText();
	if (ItemText(ID_JOIN_CODE).empty() && ValidCode(clip) && clip != ItemText(ID_CODE))
		SetDlgItemTextA(g_hud, ID_JOIN_CODE, clip.c_str());
}

static void DoHost()
{
	int players = 2 + (int)SendDlgItemMessageW(g_hud, ID_PLAYERS, CB_GETCURSEL, 0, 0);
	std::string map = ItemText(ID_MAP);
	if (map.size() > 4 && !_stricmp(map.c_str() + map.size() - 4, ".bsp"))
		map.resize(map.size() - 4);
	if (map.empty() || map.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
			std::string::npos)
	{
		Status(L"Nome de mapa inválido.");
		return;
	}
	SetDlgItemTextA(g_hud, ID_CODE, "");
	DeleteFileA((g_tunnelDir + "\\codigo.txt").c_str());
	bool tunnel = StartTunnel("host");
	char cmd[160];
	sprintf(cmd, "disconnect\nsv_lan 0\nmaxplayers %d\nmap %s\n", players, map.c_str());
	Queue(cmd);
	if (!tunnel)
	{
		Status(L"Servidor aberto, mas o túnel não foi encontrado. Rode o instalador de novo (opção 1).");
		return;
	}
	Status(L"Abrindo o servidor e o túnel da Steam...");
	g_codeStart = GetTickCount();
	SetTimer(g_hud, TIMER_CODE, 250, NULL);
}

// o tunel grava o codigo em codigo.txt assim que fala com a Steam
static void CheckCode()
{
	std::string code;
	FILE* f = fopen((g_tunnelDir + "\\codigo.txt").c_str(), "r");
	if (f)
	{
		char buf[32] = {};
		fgets(buf, sizeof(buf), f);
		fclose(f);
		code = buf;
		while (!code.empty() && (code.back() == '\n' || code.back() == '\r'))
			code.pop_back();
	}
	if (ValidCode(code))
	{
		KillTimer(g_hud, TIMER_CODE);
		SetDlgItemTextA(g_hud, ID_CODE, code.c_str());
		CopyText(code);
		Status(L"Servidor criado! Código copiado: mande para o seu amigo. O mapa começa quando todos entrarem.");
	}
	else if (!TunnelAlive())
	{
		KillTimer(g_hud, TIMER_CODE);
		Status(L"O túnel fechou. A Steam está aberta e logada? O servidor continua aberto.");
	}
	else if (GetTickCount() - g_codeStart > 20000)
	{
		KillTimer(g_hud, TIMER_CODE);
		Status(L"O túnel não respondeu. Veja a janela dele na barra de tarefas.");
	}
}

static void DoJoin()
{
	std::string code = ItemText(ID_JOIN_CODE);
	if (!ValidCode(code))
	{
		Status(L"Código inválido. É o número grande que aparece para quem hospeda.");
		return;
	}
	if (!StartTunnel("join " + code))
	{
		Status(L"Não achei o túnel. Rode o instalador de novo (opção 1).");
		return;
	}
	Status(L"Conectando ao host pela Steam...");
	g_codeStart = GetTickCount();
	SetTimer(g_hud, TIMER_CONNECT, 250, NULL);
}

// o tunel grava conectado.txt quando o tunel do host responde; so entao vale mandar o connect
static void Connect()
{
	if (!TunnelAlive())
	{
		KillTimer(g_hud, TIMER_CONNECT);
		Status(L"O túnel fechou. Confira o código e se a Steam está aberta.");
		return;
	}
	DWORD waited = GetTickCount() - g_codeStart;
	if (GetFileAttributesA((g_tunnelDir + "\\conectado.txt").c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		if (waited < 45000)
		{
			if (waited > 15000)
				Status(L"O host ainda não respondeu. O túnel dele está aberto? Os dois estão na mesma versão?");
			return;
		}
		Status(L"O túnel do host não respondeu. Tentando entrar mesmo assim; veja a janela do túnel.");
	}
	KillTimer(g_hud, TIMER_CONNECT);
	char cmd[64];
	sprintf(cmd, "connect 127.0.0.1:%d\n", TUNNEL_PORT);
	Queue(cmd);
	Status(L"Entrando... Se não conectar, aperte Entrar de novo.");
	ShowHud(false);
}

// ---------------------------------------------------------------- painel nativo do co-op

// O client.dll do CoF so desenha a lista do co-op ("Nome - Health - Ping", com o icone de quem tem
// seringa) quando o nome do mapa tem "cof_" (os mapas c_cof_*). Trocamos o texto que ele procura
// pelo nosso: vazio (vale qualquer mapa) quando o jogo tem mais de um jogador, "cof_" no single player.
static char g_coopTag[8] = "cof_";
static const DWORD RVA_PANEL_PUSH = 0x3c66c;  // push "cof_" antes do strstr(GetLevelName(), ...)
static const DWORD RVA_COF_STR = 0x14688c;
static const DWORD RVA_GETMAXCLIENTS = 0x1b2698; // gEngfuncs.GetMaxClients (copia do client.dll)

static void CoopPanelThink()
{
	static bool tried, patched;
	static BYTE* base;
	if (!tried)
	{
		base = (BYTE*)GetModuleHandleA("client.dll");
		if (!base)
			return;
		tried = true;
		BYTE* push = base + RVA_PANEL_PUSH;
		if (push[0] != 0x68 || *(DWORD*)(push + 1) != (DWORD)(base + RVA_COF_STR) ||
			memcmp(base + RVA_COF_STR, "cof_", 5))
			return; // outra versao do client.dll
		DWORD old;
		if (!VirtualProtect(push + 1, 4, PAGE_EXECUTE_READWRITE, &old))
			return;
		*(DWORD*)(push + 1) = (DWORD)g_coopTag;
		VirtualProtect(push + 1, 4, old, &old);
		FlushInstructionCache(GetCurrentProcess(), push, 5);
		patched = true;
	}
	if (!patched)
		return;
	typedef int (*GetMaxClients_t)();
	GetMaxClients_t maxClients = *(GetMaxClients_t*)(base + RVA_GETMAXCLIENTS);
	bool mp = maxClients && maxClients() > 1;
	if (mp && g_coopTag[0])
		g_coopTag[0] = 0;
	else if (!mp && !g_coopTag[0])
		g_coopTag[0] = 'c'; // "of_" continua no buffer
}

static void Poll()
{
	static bool homeDown;
	CoopPanelThink();
	AttachGame();
	bool down = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
	if (down && !homeDown)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		if (pid == GetCurrentProcessId())
			ShowHud(!IsWindowVisible(g_hud));
	}
	homeDown = down;
}

static LRESULT CALLBACK HudProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m)
	{
	case WM_TIMER:
		if (w == TIMER_POLL)
			Poll();
		else if (w == TIMER_CODE)
			CheckCode();
		else if (w == TIMER_CONNECT)
			Connect();
		return 0;
	case WM_COMMAND:
		switch (LOWORD(w))
		{
		case ID_HOST:
			DoHost();
			break;
		case ID_JOIN:
			DoJoin();
			break;
		case ID_GO: // o mapa espera o grupo antes de rodar as cutscenes; aqui o host comeca sem esperar
			Queue("cofmp_go\n");
			Status(L"Começando sem esperar os outros jogadores.");
			ShowHud(false);
			break;
		case ID_COPY:
			if (!ItemText(ID_CODE).empty())
			{
				CopyText(ItemText(ID_CODE));
				Status(L"Código copiado.");
			}
			break;
		case ID_PASTE:
			SetDlgItemTextA(h, ID_JOIN_CODE, ClipboardText().c_str());
			break;
		case IDOK: // Enter
			if (GetFocus() == GetDlgItem(h, ID_JOIN_CODE))
				DoJoin();
			else if (GetFocus() == GetDlgItem(h, ID_MAP) || GetParent(GetFocus()) == GetDlgItem(h, ID_MAP))
				DoHost();
			break;
		case IDCANCEL: // Esc
			ShowHud(false);
			break;
		}
		return 0;
	case WM_CLOSE:
		ShowHud(false);
		return 0;
	}
	return DefWindowProcW(h, m, w, l);
}

static HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
	HFONT font = g_font)
{
	HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_hud, (HMENU)(INT_PTR)id,
		NULL, NULL);
	SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
	return c;
}

static void CreateHud()
{
	NONCLIENTMETRICSW ncm = { sizeof(ncm) };
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
	g_font = CreateFontIndirectW(&ncm.lfMessageFont);
	LOGFONTW lf = ncm.lfMessageFont;
	lf.lfHeight = lf.lfHeight * 3 / 2;
	lf.lfWeight = FW_BOLD;
	g_bold = CreateFontIndirectW(&lf);

	WNDCLASSW wc = {};
	wc.lpfnWndProc = HudProc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = L"cofmp_hud";
	RegisterClassW(&wc);
	const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
	RECT r = { 0, 0, 380, 352 };
	AdjustWindowRectEx(&r, style, FALSE, ex);
	g_hud = CreateWindowExW(ex, wc.lpszClassName, L"Cry of Fear co-op v" COFMP_VERSION, style, 0, 0, r.right - r.left,
		r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);

	const DWORD S = SS_LEFT, SC = SS_LEFT | SS_CENTERIMAGE, B = WS_TABSTOP | BS_PUSHBUTTON,
				E = WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
	Ctl(L"STATIC", L"Cry of Fear — co-op", S, 12, 10, 356, 26, 0, g_bold);

	Ctl(L"BUTTON", L"Hospedar", BS_GROUPBOX, 10, 40, 360, 128, 0);
	Ctl(L"STATIC", L"Jogadores:", SC, 22, 62, 70, 23, 0);
	HWND players = Ctl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, 95, 62, 55, 120, ID_PLAYERS);
	for (const wchar_t* n : { L"2", L"3", L"4" })
		SendMessageW(players, CB_ADDSTRING, 0, (LPARAM)n);
	SendMessageW(players, CB_SETCURSEL, 0, 0); // o mapa espera esse numero de jogadores para comecar
	Ctl(L"STATIC", L"Mapa:", SC, 162, 62, 40, 23, 0);
	HWND map = Ctl(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_SORT, 205, 62,
		150, 300, ID_MAP);
	WIN32_FIND_DATAA fd;
	HANDLE fh = FindFirstFileA((g_cry + "\\maps\\c_*.bsp").c_str(), &fd);
	if (fh != INVALID_HANDLE_VALUE)
	{
		do
		{
			*strrchr(fd.cFileName, '.') = 0;
			SendMessageA(map, CB_ADDSTRING, 0, (LPARAM)fd.cFileName);
		} while (FindNextFileA(fh, &fd));
		FindClose(fh);
	}
	SetWindowTextA(map, "c_intro"); // comeco do jogo, com a cutscene
	Ctl(L"BUTTON", L"Criar servidor", B, 22, 94, 200, 28, ID_HOST);
	Ctl(L"BUTTON", L"Começar agora", B, 228, 94, 127, 28, ID_GO);
	Ctl(L"STATIC", L"Código:", SC, 22, 132, 50, 23, 0);
	Ctl(L"EDIT", L"", E | ES_READONLY, 75, 132, 200, 23, ID_CODE);
	Ctl(L"BUTTON", L"Copiar", B, 282, 132, 73, 23, ID_COPY);

	Ctl(L"BUTTON", L"Entrar no jogo de um amigo", BS_GROUPBOX, 10, 178, 360, 100, 0);
	Ctl(L"STATIC", L"Código do host:", SC, 22, 200, 95, 23, 0);
	Ctl(L"EDIT", L"", E | ES_NUMBER, 120, 200, 155, 23, ID_JOIN_CODE);
	Ctl(L"BUTTON", L"Colar", B, 282, 200, 73, 23, ID_PASTE);
	Ctl(L"BUTTON", L"Entrar", B, 22, 232, 333, 28, ID_JOIN);

	Ctl(L"STATIC", L"", S, 12, 288, 356, 36, ID_STATUS);
	Ctl(L"STATIC", L"Home abre e fecha esta janela. Os dois usam a Steam aberta.", S, 12, 328, 296, 18, 0);
	Ctl(L"STATIC", L"v" COFMP_VERSION, SS_RIGHT, 310, 328, 58, 18, 0);
	SetTimer(g_hud, TIMER_POLL, 30, NULL);
}

static DWORD WINAPI HudThread(void*)
{
	char path[MAX_PATH];
	HMODULE self;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&HudThread, &self);
	GetModuleFileNameA(self, path, sizeof(path));
	*strrchr(path, '\\') = 0; // ...\cryoffear\cl_dlls
	g_tunnelDir = std::string(path) + "\\cofmp_tunel";
	*strrchr(path, '\\') = 0;
	g_cry = path;

	g_job = CreateJobObjectA(NULL, NULL);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
	li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (g_job && !SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof(li)))
	{
		CloseHandle(g_job);
		g_job = NULL;
	}

	// controles com o visual do Windows: manifesto embutido (recurso 2) desta DLL
	ACTCTXW ac = { sizeof(ac) };
	ac.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
	ac.hModule = self;
	ac.lpResourceName = MAKEINTRESOURCEW(2);
	HANDLE actx = CreateActCtxW(&ac);
	ULONG_PTR cookie;
	if (actx != INVALID_HANDLE_VALUE)
		ActivateActCtx(actx, &cookie);
	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
	InitCommonControlsEx(&icc);

	CreateHud();
	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0)
	{
		if (IsDialogMessageW(g_hud, &msg)) // Tab, Enter e Esc
			continue;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return 0;
}

void Hud_Start(HudCmd_t cmd)
{
	static bool started;
	// so no jogo: o servidor dedicado (swds.dll) nao tem janela
	if (started || (!GetModuleHandleA("hw.dll") && !GetModuleHandleA("sw.dll")))
		return;
	started = true;
	InitializeCriticalSection(&g_lock);
	g_cmd = cmd;
	CloseHandle(CreateThread(NULL, 0, HudThread, NULL, 0, NULL));
}
