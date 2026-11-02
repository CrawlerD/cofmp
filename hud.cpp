// hud.cpp - janela do co-op dentro do jogo (tecla Home): hospedar ou entrar num servidor pelo tunel da Steam.
//
// Roda numa thread propria do cofmp.dll no processo do jogo (o engine carrega o gamedll ja no menu).
// Os comandos do console (map, connect...) precisam rodar na thread principal: ficam numa fila que a
// janela do jogo (subclassificada) esvazia ao receber WM_HUD_CMD, e o StartFrame tambem esvazia.
// O tunel (CoF_Tunel.exe + steam_api.dll) e copiado pelo instalador para cryoffear\Mod coop\tunel.

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <algorithm>
#include "hud.h"
#include "logfile.h"

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

enum { ID_PLAYERS = 100, ID_MAP, ID_HOST, ID_CODE, ID_COPY, ID_JOIN_CODE, ID_PASTE, ID_JOIN, ID_STATUS, ID_GO, ID_CHANGE, ID_SHOW_SIMON,
	ID_SHOW_COPS, ID_SHOW_COMMUNITY };
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

// ---------------------------------------------------------------- Steam: "Entrar no jogo" da lista de amigos
//
// O host publica no rich presence "connect" = "-cofmpjoin <codigo>"; a Steam mostra "Entrar no jogo" para os amigos.
// Com o jogo aberto, o clique chega como GameRichPresenceJoinRequested_t; com ele fechado, a Steam abre o jogo com o
// texto na linha de comando (o CoFLaunchApp.exe repassa os argumentos para o cof.exe).
// Usa o steam_api.dll que o proprio jogo ja carregou e iniciou (ISteamFriends013, indices conferidos com a Steam).

struct CSteamID { unsigned long long v; };

class CCallbackBase // mesmo layout do SDK, igual ao do tunel
{
public:
	virtual void Run(void* param) = 0;
	virtual void Run(void* param, bool ioFailure, unsigned long long call) = 0;
	virtual int GetCallbackSizeBytes() = 0;
	unsigned char m_nCallbackFlags = 0;
	int m_iCallback = 0;
};

enum { CB_JOIN_REQUESTED = 337, FRIEND_IMMEDIATE = 4 };
enum { VT_FRIEND_COUNT = 3, VT_FRIEND_BY_INDEX = 4, VT_SET_RP = 36, VT_CLEAR_RP = 37, VT_GET_FRIEND_RP = 38 };
static const char JOIN_ARG[] = "-cofmpjoin";

static void (__cdecl* g_runCallbacks)();
static bool g_presence; // o rich presence "connect" esta publicado

static void** SteamFriends()
{
	HMODULE api = GetModuleHandleA("steam_api.dll");
	typedef void** (__cdecl * Get_t)();
	Get_t get = api ? (Get_t)GetProcAddress(api, "SteamFriends") : NULL;
	return get ? get() : NULL;
}

static void SetPresence(const std::string& code)
{
	void** f = SteamFriends();
	if (!f)
		return;
	void** vt = *(void***)f;
	typedef bool(__thiscall * Set_t)(void*, const char*, const char*);
	typedef void(__thiscall * Clear_t)(void*);
	if (code.empty())
	{
		((Clear_t)vt[VT_CLEAR_RP])(f);
		g_presence = false;
		return;
	}
	std::string connect = std::string(JOIN_ARG) + " " + code;
	g_presence = ((Set_t)vt[VT_SET_RP])(f, "connect", connect.c_str());
	((Set_t)vt[VT_SET_RP])(f, "status", "Hospedando o co-op");
	LogFile_Write("hud", g_presence ? "rich presence publicado (Entrar no jogo pela Steam)" : "rich presence recusado");
}

// codigo depois de "-cofmpjoin" (linha de comando ou o connect do rich presence)
static std::string JoinArg(const char* s)
{
	const char* p = s ? strstr(s, JOIN_ARG) : NULL;
	if (!p)
		return "";
	p += sizeof(JOIN_ARG) - 1;
	p += strspn(p, " \t\"");
	return std::string(p, strspn(p, "0123456789"));
}

// amigo no Cry of Fear com o servidor do co-op aberto: o codigo dele
static std::string HostingFriend()
{
	void** f = SteamFriends();
	if (!f)
		return "";
	void** vt = *(void***)f;
	typedef int(__thiscall * Count_t)(void*, int);
	// metodo que devolve struct (CSteamID) recebe o endereco de retorno como 1o argumento depois do this;
	// um ponteiro de funcao comum devolveria em edx:eax e a Steam escreveria no endereco "k"
	typedef CSteamID*(__thiscall * ByIndex_t)(void*, CSteamID*, int, int);
	typedef const char*(__thiscall * GetRP_t)(void*, CSteamID, const char*);
	int n = ((Count_t)vt[VT_FRIEND_COUNT])(f, FRIEND_IMMEDIATE);
	for (int k = 0; k < n; k++)
	{
		CSteamID id = {};
		((ByIndex_t)vt[VT_FRIEND_BY_INDEX])(f, &id, k, FRIEND_IMMEDIATE);
		std::string code = JoinArg(((GetRP_t)vt[VT_GET_FRIEND_RP])(f, id, "connect"));
		if (!code.empty())
			return code;
	}
	return "";
}

static void JoinFromSteam(const std::string& code, const char* from);

class JoinRequested : public CCallbackBase
{
public:
	void Run(void* p) override
	{
		// GameRichPresenceJoinRequested_t: CSteamID do amigo + char connect[256]
		const char* connect = (const char*)p + 8;
		JoinFromSteam(JoinArg(std::string(connect, strnlen(connect, 256)).c_str()), "lista de amigos");
	}
	void Run(void* p, bool, unsigned long long) override { Run(p); }
	int GetCallbackSizeBytes() override { return 8 + 256; }
};

static void SteamThink()
{
	static bool tried;
	if (!tried)
	{
		HMODULE api = GetModuleHandleA("steam_api.dll");
		if (!api || !SteamFriends())
			return; // o jogo ainda nao iniciou a Steam
		tried = true;
		typedef void(__cdecl * Register_t)(CCallbackBase*, int);
		Register_t reg = (Register_t)GetProcAddress(api, "SteamAPI_RegisterCallback");
		g_runCallbacks = (void(__cdecl*)())GetProcAddress(api, "SteamAPI_RunCallbacks");
		static JoinRequested onJoin;
		if (reg && g_runCallbacks)
			reg(&onJoin, CB_JOIN_REQUESTED);
		else
			g_runCallbacks = NULL;
	}
	// o jogo nao chama o RunCallbacks deste steam_api.dll; os callbacks rodam aqui, na thread do HUD
	if (g_runCallbacks)
		g_runCallbacks();
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
	// senao, o de um amigo que esta hospedando agora
	if (ItemText(ID_JOIN_CODE).empty())
		SetDlgItemTextA(g_hud, ID_JOIN_CODE, HostingFriend().c_str());
}

// nome do mapa escolhido, sem ".bsp"; vazio (e aviso no status) se for invalido
static std::string MapName()
{
	std::string map = ItemText(ID_MAP);
	if (map.size() > 4 && !_stricmp(map.c_str() + map.size() - 4, ".bsp"))
		map.resize(map.size() - 4);
	if (map.empty() || map.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
			std::string::npos)
	{
		Status(L"Nome de mapa inválido.");
		return "";
	}
	return map;
}

static void DoHost()
{
	int players = 2 + (int)SendDlgItemMessageW(g_hud, ID_PLAYERS, CB_GETCURSEL, 0, 0);
	std::string map = MapName();
	if (map.empty())
		return;
	SetDlgItemTextA(g_hud, ID_CODE, "");
	DeleteFileA((g_tunnelDir + "\\codigo.txt").c_str());
	bool tunnel = StartTunnel("host");
	// sv_lan 1: servidor sem VAC (o amigo chega pelo tunel como 127.0.0.1, que conta como LAN);
	// sv_lan_rate: o padrao (20000) limita a banda de quem vem pela LAN
	char cmd[200];
	sprintf(cmd, "disconnect\nsv_lan 1\nsv_lan_rate 100000\nmaxplayers %d\nmap %s\n", players, map.c_str());
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

// so o mapa: o servidor, o tunel e quem esta conectado continuam (cofmp_map leva o inventario junto)
static void DoChangeMap()
{
	std::string map = MapName();
	if (map.empty())
		return;
	Queue(("cofmp_map " + map + "\n").c_str());
	Status(L"Trocando de mapa. Só funciona com o servidor aberto por você (Criar servidor).");
	ShowHud(false);
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
		SetPresence(code);
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
	if (g_presence) // deixou de hospedar
		SetPresence("");
	Status(L"Conectando ao host pela Steam...");
	g_codeStart = GetTickCount();
	SetTimer(g_hud, TIMER_CONNECT, 250, NULL);
}

// "Entrar no jogo" na Steam: preenche o codigo e entra sozinho
static void JoinFromSteam(const std::string& code, const char* from)
{
	char msg[160];
	_snprintf(msg, sizeof(msg) - 1, "entrar pela Steam (%s): codigo '%s'", from, code.c_str());
	msg[sizeof(msg) - 1] = 0;
	LogFile_Write("hud", msg);
	if (!ValidCode(code))
		return;
	SetDlgItemTextA(g_hud, ID_JOIN_CODE, code.c_str());
	ShowHud(true);
	DoJoin();
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
	// o padrao do jogo (cl_cmdrate 30, rate 25000) atrasa os comandos e corta pacotes
	char cmd[128];
	sprintf(cmd, "rate 100000\ncl_cmdrate 60\ncl_updaterate 30\nconnect 127.0.0.1:%d\n", TUNNEL_PORT);
	Queue(cmd);
	Status(L"Entrando... Se não conectar, aperte Entrar de novo.");
	ShowHud(false);
}

// ---------------------------------------------------------------- painel nativo do co-op

// O client.dll do CoF so desenha a lista do co-op ("Nome - Health - Ping", com o icone de quem tem
// seringa) quando o nome do mapa tem "cof_" (os mapas c_cof_*). Trocamos o texto que ele procura
// pelo nosso: vazio (vale qualquer mapa) quando o jogo tem mais de um jogador, "cof_" no single player.
static char g_coopTag[8] = "cof_";
static bool g_inMP; // o cliente esta num jogo com mais de um jogador
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
	g_inMP = mp;
	if (mp && g_coopTag[0])
		g_coopTag[0] = 0;
	else if (!mp && !g_coopTag[0])
		g_coopTag[0] = 'c'; // "of_" continua no buffer
}

static void Poll()
{
	static bool homeDown, enterDown, cmdLineDone;
	static DWORD attachedAt;
	CoopPanelThink();
	AttachGame();
	SteamThink();
	if (g_presence && !TunnelAlive()) // o tunel do host fechou: tira o "Entrar no jogo"
		SetPresence("");
	// jogo aberto pela Steam com "-cofmpjoin <codigo>": entra assim que o menu aparece
	if (!cmdLineDone && g_game)
	{
		if (!attachedAt)
			attachedAt = GetTickCount();
		else if (GetTickCount() - attachedAt > 3000)
		{
			cmdLineDone = true;
			std::string code = JoinArg(GetCommandLineA());
			if (!code.empty())
				JoinFromSteam(code, "linha de comando");
		}
	}
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	bool inGame = pid == GetCurrentProcessId();
	bool down = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
	if (down && !homeDown && inGame)
		ShowHud(!IsWindowVisible(g_hud));
	homeDown = down;
	// Enter pula a cutscene, como no SP; no MP o hl.dll nao le o teclado, entao o pedido vai para o servidor
	bool enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
	if (enter && !enterDown && inGame && fg != g_hud && g_inMP)
		Queue("cmd cofmp_skip\n");
	enterDown = enter;
}

static void FillMaps();

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
		case ID_CHANGE:
			DoChangeMap();
			break;
		case ID_GO: // o mapa espera o grupo antes de rodar as cutscenes; aqui o host comeca sem esperar
			Queue("cofmp_go\n");
			Status(L"Começando sem esperar os outros jogadores.");
			ShowHud(false);
			break;
		case ID_SHOW_SIMON:
		case ID_SHOW_COPS:
		case ID_SHOW_COMMUNITY:
			FillMaps();
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

// Lista de mapas do Home, separada em tres grupos que o jogador marca: a campanha do Simon (os mapas que o c_intro
// alcanca pelas saidas, mais os do Doctor), a dos policiais (c_cof_*) e os mapas da comunidade (todo o resto).
static const char* const SIMON_MAPS[] = { "c_apartment1", "c_apartment2", "c_apartment3", "c_apartment4",
	"c_apartment5", "c_apartment3sick", "c_apartment4sick", "c_apartmentsick", "c_asylum1", "c_asylum1dark",
	"c_asylum1day", "c_asylum2", "c_asylum2dark", "c_asylumb", "c_attic", "c_avlopp", "c_basement", "c_bridge",
	"c_broscene", "c_buscity", "c_buscity2", "c_city", "c_city2", "c_college1", "c_college1dark", "c_college2",
	"c_down", "c_ending1", "c_ending1b", "c_ending1c", "c_ending2", "c_ending2b", "c_ending2c", "c_ending3",
	"c_ending3b", "c_ending3c", "c_ending4", "c_ending4b", "c_ending5", "c_ending5b", "c_ending5c", "c_forest1",
	"c_forest2", "c_forest3", "c_forest4", "c_forest5", "c_forest6", "c_forest7", "c_forestday", "c_fsewer1",
	"c_hamn", "c_hamn2", "c_hamn3", "c_intro", "c_lake", "c_nightmare", "c_park", "c_park2", "c_park3", "c_roofs",
	"c_roofs2", "c_sewer", "c_sewer2", "c_sewer3", "c_stad", "c_start", "c_start2", "c_subtunnel1", "c_subtunnel2",
	"c_subway1st1", "c_subway2st1", "c_subway2st2", "c_subway2st3", "c_subwaysick1", "c_subwaysick2", "c_trainride",
	"c_trainride2", "c_trainscene", "c_village1", "c_village2", "c_village3", "c_doc_apartment3sick",
	"c_doc_apartment4sick", "c_doc_appark", "c_doc_basement", "c_doc_city", "c_doc_ending", "c_doc_sewer",
	"c_doc_sewer2", "c_doc_sewer3" };
static const char* const MENU_MAPS[] = { "c_game_menu1", "c_loadgame", "c_difficulty_settings", "c_unlockables" };

static int MapGroup(const char* name) // ID_SHOW_*, ou 0 para os mapas de menu do jogo
{
	for (const char* m : MENU_MAPS)
		if (!_stricmp(name, m))
			return 0;
	for (const char* m : SIMON_MAPS)
		if (!_stricmp(name, m))
			return ID_SHOW_SIMON;
	return _strnicmp(name, "c_cof_", 6) ? ID_SHOW_COMMUNITY : ID_SHOW_COPS;
}

static void FillMaps()
{
	HWND map = GetDlgItem(g_hud, ID_MAP);
	std::string keep = ItemText(ID_MAP); // o que esta escrito fica, mesmo se o mapa sair da lista
	SendMessageW(map, CB_RESETCONTENT, 0, 0);
	WIN32_FIND_DATAA fd;
	HANDLE fh = FindFirstFileA((g_cry + "\\maps\\c_*.bsp").c_str(), &fd);
	if (fh != INVALID_HANDLE_VALUE)
	{
		do
		{
			*strrchr(fd.cFileName, '.') = 0;
			int group = MapGroup(fd.cFileName);
			if (group && IsDlgButtonChecked(g_hud, group) == BST_CHECKED)
				SendMessageA(map, CB_ADDSTRING, 0, (LPARAM)fd.cFileName);
		} while (FindNextFileA(fh, &fd));
		FindClose(fh);
	}
	SetWindowTextA(map, keep.c_str());
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
	RECT r = { 0, 0, 380, 380 };
	AdjustWindowRectEx(&r, style, FALSE, ex);
	g_hud = CreateWindowExW(ex, wc.lpszClassName, L"Cry of Fear co-op v" COFMP_VERSION, style, 0, 0, r.right - r.left,
		r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);

	const DWORD S = SS_LEFT, SC = SS_LEFT | SS_CENTERIMAGE, B = WS_TABSTOP | BS_PUSHBUTTON,
				E = WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
	Ctl(L"STATIC", L"Cry of Fear — co-op", S, 12, 10, 356, 26, 0, g_bold);

	Ctl(L"BUTTON", L"Hospedar", BS_GROUPBOX, 10, 40, 360, 156, 0);
	Ctl(L"STATIC", L"Jogadores:", SC, 22, 62, 70, 23, 0);
	HWND players = Ctl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, 95, 62, 55, 120, ID_PLAYERS);
	for (const wchar_t* n : { L"2", L"3", L"4" })
		SendMessageW(players, CB_ADDSTRING, 0, (LPARAM)n);
	SendMessageW(players, CB_SETCURSEL, 0, 0); // o mapa espera esse numero de jogadores para comecar
	Ctl(L"STATIC", L"Mapa:", SC, 162, 62, 40, 23, 0);
	HWND map = Ctl(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_SORT, 205, 62,
		150, 300, ID_MAP);
	const DWORD C = WS_TABSTOP | BS_AUTOCHECKBOX;
	Ctl(L"STATIC", L"Mostrar:", SC, 22, 92, 70, 20, 0);
	Ctl(L"BUTTON", L"Simon", C, 95, 92, 62, 20, ID_SHOW_SIMON);
	Ctl(L"BUTTON", L"Policiais", C, 162, 92, 75, 20, ID_SHOW_COPS);
	Ctl(L"BUTTON", L"Comunidade", C, 242, 92, 113, 20, ID_SHOW_COMMUNITY);
	CheckDlgButton(g_hud, ID_SHOW_SIMON, BST_CHECKED);
	SetWindowTextA(map, "c_intro"); // comeco do jogo, com a cutscene
	FillMaps();
	Ctl(L"BUTTON", L"Criar servidor", B, 22, 122, 112, 28, ID_HOST);
	Ctl(L"BUTTON", L"Trocar mapa", B, 139, 122, 104, 28, ID_CHANGE);
	Ctl(L"BUTTON", L"Começar agora", B, 248, 122, 107, 28, ID_GO);
	Ctl(L"STATIC", L"Código:", SC, 22, 160, 50, 23, 0);
	Ctl(L"EDIT", L"", E | ES_READONLY, 75, 160, 200, 23, ID_CODE);
	Ctl(L"BUTTON", L"Copiar", B, 282, 160, 73, 23, ID_COPY);

	Ctl(L"BUTTON", L"Entrar no jogo de um amigo", BS_GROUPBOX, 10, 206, 360, 100, 0);
	Ctl(L"STATIC", L"Código do host:", SC, 22, 228, 95, 23, 0);
	Ctl(L"EDIT", L"", E | ES_NUMBER, 120, 228, 155, 23, ID_JOIN_CODE);
	Ctl(L"BUTTON", L"Colar", B, 282, 228, 73, 23, ID_PASTE);
	Ctl(L"BUTTON", L"Entrar", B, 22, 260, 333, 28, ID_JOIN);

	Ctl(L"STATIC", L"", S, 12, 316, 356, 36, ID_STATUS);
	Ctl(L"STATIC", L"Home abre e fecha esta janela. Os dois usam a Steam aberta.", S, 12, 356, 296, 18, 0);
	Ctl(L"STATIC", L"v" COFMP_VERSION, SS_RIGHT, 310, 356, 58, 18, 0);
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
	*strrchr(path, '\\') = 0;
	g_cry = path;
	g_tunnelDir = g_cry + "\\" COFMP_TUNNEL_DIR;
	LogFile_Open((g_cry + "\\" COFMP_DIR).c_str()); // o g_logPath do logfile.h e de cada .cpp

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
