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
	ID_SHOW_COPS, ID_SHOW_COMMUNITY, ID_NOCLIP, ID_GOD, ID_TARGET, ID_TP, ID_SPEC, ID_SPEC_STOP, ID_SAVE, ID_SAVES,
	ID_LOAD };
enum { WM_HUD_CMD = WM_APP + 0x43 };
enum { TIMER_POLL = 1, TIMER_CODE, TIMER_CONNECT, TIMER_SAVES };
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

static void FillTargets();
static void FillSaves();

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
	FillTargets();
	FillSaves();
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

// start: "map c_intro" ou "load coop_..." (carregar uma partida salva)
static void HostWith(const std::string& start)
{
	int players = 2 + (int)SendDlgItemMessageW(g_hud, ID_PLAYERS, CB_GETCURSEL, 0, 0);
	SetDlgItemTextA(g_hud, ID_CODE, "");
	DeleteFileA((g_tunnelDir + "\\codigo.txt").c_str());
	bool tunnel = StartTunnel("host");
	// sv_lan 1: servidor sem VAC (o amigo chega pelo tunel como 127.0.0.1, que conta como LAN);
	// sv_lan_rate: o padrao (20000) limita a banda de quem vem pela LAN
	char cmd[200];
	sprintf(cmd, "disconnect\nsv_lan 1\nsv_lan_rate 100000\nmaxplayers %d\n%s\n", players, start.c_str());
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

static void DoHost()
{
	std::string map = MapName();
	if (!map.empty())
		HostWith("map " + map);
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

// ---------------------------------------------------------------- Esc e maquinas de save no co-op
//
// O GameUI.dll e o client.dll recebem do engine uma copia da tabela de funcoes do cliente (cl_enginefunc_t) e
// mandam comandos pelo pfnClientCmd dela (+0x50). Trocamos esse ponteiro nas copias:
//  - GameUI: "setpause"/"unpause" ao abrir/fechar o menu viram "cmd cofmp_menu 1/0" (o servidor so pausa com
//    todos no menu);
//  - client.dll: o "savehack cofsaveN" da maquina de save vira "cmd cofmp_save maquina" (save co-op no host, sem
//    tocar nos cofsave1..5 do single player). O client.dll grava o saveinfoN.cof do slot mesmo assim: o
//    original volta logo depois.
typedef int (*ClientCmd_t)(char* cmd);
static const DWORD RVA_UI_ENGFUNCS = 0xf9c58, RVA_UI_ENGFUNCS_PTR = 0xf9e8c, RVA_UI_SETPAUSE = 0xe40a0;
static const DWORD RVA_CL_ENGFUNCS = RVA_GETMAXCLIENTS - 36 * 4;
static const int SLOT_SERVERCMD = 0x4c / 4, SLOT_CLIENTCMD = 0x50 / 4, SLOT_GETPLAYERINFO = 0x54 / 4,
				 SLOT_GETMAXCLIENTS = 36;
static ClientCmd_t g_uiClientCmd, g_clClientCmd, g_clServerCmd;
static DWORD g_saveInfoAt; // quando devolver os saveinfo*.cof do single player
static std::string g_saveInfo[6];
static bool g_saveInfoRead;

static int UiClientCmd(char* cmd)
{
	if (g_inMP && cmd && (!strcmp(cmd, "setpause") || !strcmp(cmd, "unpause")))
		return g_uiClientCmd((char*)(cmd[0] == 's' ? "cmd cofmp_menu 1" : "cmd cofmp_menu 0"));
	return g_uiClientCmd(cmd);
}

static bool MachineSave(const char* cmd)
{
	if (!g_inMP || !cmd || _strnicmp(cmd, "savehack", 8))
		return false;
	g_clClientCmd((char*)"cmd cofmp_save maquina");
	g_saveInfoAt = GetTickCount() + 1500;
	LogFile_Write("jogo", "maquina de save no co-op: pedido mandado ao host");
	return true;
}

static int ClClientCmd(char* cmd)
{
	return MachineSave(cmd) ? 1 : g_clClientCmd(cmd);
}

static int ClServerCmd(char* cmd)
{
	return MachineSave(cmd) ? 1 : g_clServerCmd(cmd);
}

static bool InModule(void* p, const char* mod)
{
	HMODULE m;
	return p &&
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)p, &m) &&
		m == GetModuleHandleA(mod);
}

static void SetSlot(void** slot, void* fn)
{
	DWORD old;
	if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old))
		return;
	*slot = fn;
	VirtualProtect(slot, 4, old, &old);
}

static void CmdHooksThink()
{
	static bool uiDone, clDone;
	const char* engine = GetModuleHandleA("hw.dll") ? "hw.dll" : "sw.dll";
	BYTE* ui = uiDone ? NULL : (BYTE*)GetModuleHandleA("GameUI.dll");
	if (ui && *(BYTE**)(ui + RVA_UI_ENGFUNCS_PTR) == ui + RVA_UI_ENGFUNCS) // o GameUI ja copiou a tabela
	{
		void** t = (void**)(ui + RVA_UI_ENGFUNCS);
		uiDone = true;
		if (memcmp(ui + RVA_UI_SETPAUSE, "setpause", 9) || !InModule(t[SLOT_CLIENTCMD], engine))
			LogFile_Write("jogo", "aviso: GameUI.dll de outra versao - o Esc de um jogador ainda pausa todos");
		else
		{
			g_uiClientCmd = (ClientCmd_t)t[SLOT_CLIENTCMD];
			SetSlot(&t[SLOT_CLIENTCMD], (void*)UiClientCmd);
		}
	}
	BYTE* cl = clDone ? NULL : (BYTE*)GetModuleHandleA("client.dll");
	if (cl)
	{
		void** t = (void**)(cl + RVA_CL_ENGFUNCS);
		if (!InModule(t[SLOT_GETMAXCLIENTS], engine)) // a tabela ainda nao foi preenchida
			return;
		clDone = true;
		if (!InModule(t[SLOT_CLIENTCMD], engine) || !InModule(t[SLOT_SERVERCMD], engine) ||
			!InModule(t[SLOT_GETPLAYERINFO], engine))
			LogFile_Write("jogo", "aviso: client.dll de outra versao - maquina de save e lista de jogadores desligadas");
		else
		{
			g_clClientCmd = (ClientCmd_t)t[SLOT_CLIENTCMD];
			g_clServerCmd = (ClientCmd_t)t[SLOT_SERVERCMD];
			SetSlot(&t[SLOT_CLIENTCMD], (void*)ClClientCmd);
			SetSlot(&t[SLOT_SERVERCMD], (void*)ClServerCmd);
		}
	}
}

// saveinfoN.cof (cryoffear\SAVE): o texto que o menu Carregar do single player mostra em cada slot
static void SaveInfoThink()
{
	if (g_inMP && !g_saveInfoRead)
	{
		g_saveInfoRead = true;
		for (int n = 1; n <= 5; n++)
		{
			g_saveInfo[n].clear();
			FILE* f = fopen((g_cry + "\\SAVE\\saveinfo" + std::to_string(n) + ".cof").c_str(), "rb");
			if (!f)
				continue;
			char buf[512];
			size_t got = fread(buf, 1, sizeof(buf), f);
			fclose(f);
			g_saveInfo[n].assign(buf, got);
		}
	}
	if (!g_inMP && !g_saveInfoAt)
		g_saveInfoRead = false;
	if (!g_saveInfoAt || (int)(GetTickCount() - g_saveInfoAt) < 0)
		return;
	g_saveInfoAt = 0;
	for (int n = 1; n <= 5; n++)
	{
		if (g_saveInfo[n].empty())
			continue;
		FILE* f = fopen((g_cry + "\\SAVE\\saveinfo" + std::to_string(n) + ".cof").c_str(), "wb");
		if (!f)
			continue;
		fwrite(g_saveInfo[n].data(), 1, g_saveInfo[n].size(), f);
		fclose(f);
	}
}

// ---------------------------------------------------------------- painel de comandos e saves

struct PlayerInfo // hud_player_info_t, com folga para versoes maiores
{
	char* name;
	short ping;
	BYTE thisplayer, spectator, packetloss;
	char* model;
	short topcolor, bottomcolor;
	unsigned __int64 steamid;
	BYTE spare[32];
};

// jogadores do servidor em que estamos, menos nos (o engine do cliente sabe pelo placar)
static void FillTargets()
{
	HWND box = GetDlgItem(g_hud, ID_TARGET);
	int keep = -1, sel = (int)SendMessageW(box, CB_GETCURSEL, 0, 0);
	if (sel >= 0)
		keep = (int)SendMessageW(box, CB_GETITEMDATA, sel, 0);
	SendMessageW(box, CB_RESETCONTENT, 0, 0);
	BYTE* cl = (BYTE*)GetModuleHandleA("client.dll");
	if (!cl || !g_clClientCmd || !g_inMP)
		return;
	void** t = (void**)(cl + RVA_CL_ENGFUNCS);
	typedef void (*GetPlayerInfo_t)(int, PlayerInfo*);
	typedef int (*GetMaxClients_t)();
	GetPlayerInfo_t info = (GetPlayerInfo_t)t[SLOT_GETPLAYERINFO];
	int max = ((GetMaxClients_t)t[SLOT_GETMAXCLIENTS])();
	bool chosen = false;
	for (int i = 1; i <= max && i <= 32; i++)
	{
		PlayerInfo pi = {};
		info(i, &pi);
		if (!pi.name || !pi.name[0] || pi.thisplayer)
			continue;
		char line[80];
		_snprintf(line, sizeof(line), "%.60s", pi.name);
		line[sizeof(line) - 1] = 0;
		int at = (int)SendMessageA(box, CB_ADDSTRING, 0, (LPARAM)line);
		SendMessageW(box, CB_SETITEMDATA, at, i);
		if (!chosen && (i == keep || keep < 0))
		{
			SendMessageW(box, CB_SETCURSEL, at, 0);
			chosen = true;
		}
	}
	if (!chosen && SendMessageW(box, CB_GETCOUNT, 0, 0) > 0)
		SendMessageW(box, CB_SETCURSEL, 0, 0);
}

static int Target()
{
	HWND box = GetDlgItem(g_hud, ID_TARGET);
	int sel = (int)SendMessageW(box, CB_GETCURSEL, 0, 0);
	if (sel < 0)
	{
		Status(L"Escolha um jogador na lista (só aparecem os que estão no seu jogo).");
		return 0;
	}
	return (int)SendMessageW(box, CB_GETITEMDATA, sel, 0);
}

static void AdminCmd(const char* what, int n)
{
	if (!g_inMP)
	{
		Status(L"Os comandos só funcionam dentro de um jogo co-op.");
		return;
	}
	char cmd[64];
	sprintf(cmd, "cmd cofmp_admin %s %d\n", what, n);
	Queue(cmd);
}

// saves co-op: <pasta do jogo>\SAVE\coop_<mapa>_<DDMM-HHMMSS>.sav, os mais novos primeiro
static std::vector<std::string> g_saves;

static std::string SaveDir()
{
	return g_cry.substr(0, g_cry.rfind('\\')) + "\\SAVE";
}

static void FillSaves()
{
	HWND box = GetDlgItem(g_hud, ID_SAVES);
	SendMessageW(box, CB_RESETCONTENT, 0, 0);
	std::vector<std::pair<unsigned __int64, std::string>> found;
	WIN32_FIND_DATAA fd;
	HANDLE fh = FindFirstFileA((SaveDir() + "\\coop_*.sav").c_str(), &fd);
	if (fh != INVALID_HANDLE_VALUE)
	{
		do
		{
			*strrchr(fd.cFileName, '.') = 0;
			found.push_back(
				{ ((unsigned __int64)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime,
					fd.cFileName });
		} while (FindNextFileA(fh, &fd));
		FindClose(fh);
	}
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	g_saves.clear();
	for (auto& f : found)
	{
		// coop_c_start_0211-174512 -> "c_start  02/11 17:45"
		std::string name = f.second.substr(5), when;
		size_t cut = name.rfind('_');
		if (cut != std::string::npos && name.size() - cut == 12)
		{
			const char* d = name.c_str() + cut + 1;
			when = std::string("  ") + d[0] + d[1] + "/" + d[2] + d[3] + " " + d[5] + d[6] + ":" + d[7] + d[8];
			name.resize(cut);
		}
		g_saves.push_back(f.second);
		SendMessageA(box, CB_ADDSTRING, 0, (LPARAM)(name + when).c_str());
	}
	if (!g_saves.empty())
		SendMessageW(box, CB_SETCURSEL, 0, 0);
}

static void DoLoad()
{
	int sel = (int)SendDlgItemMessageW(g_hud, ID_SAVES, CB_GETCURSEL, 0, 0);
	if (sel < 0 || sel >= (int)g_saves.size())
	{
		Status(L"Nenhuma partida co-op salva neste PC ainda.");
		return;
	}
	HostWith("load " + g_saves[sel]);
}

static void Poll()
{
	static bool homeDown, enterDown, cmdLineDone;
	static DWORD attachedAt;
	CoopPanelThink();
	CmdHooksThink();
	SaveInfoThink();
	if (!g_inMP && (IsDlgButtonChecked(g_hud, ID_NOCLIP) || IsDlgButtonChecked(g_hud, ID_GOD)))
	{
		CheckDlgButton(g_hud, ID_NOCLIP, BST_UNCHECKED); // saiu do jogo: o servidor novo comeca sem eles
		CheckDlgButton(g_hud, ID_GOD, BST_UNCHECKED);
	}
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
		else if (w == TIMER_SAVES)
		{
			KillTimer(h, TIMER_SAVES);
			FillSaves();
		}
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
		case ID_NOCLIP:
		case ID_GOD:
			AdminCmd(LOWORD(w) == ID_NOCLIP ? "noclip" : "god", IsDlgButtonChecked(h, LOWORD(w)) == BST_CHECKED);
			break;
		case ID_TARGET:
			if (HIWORD(w) == CBN_DROPDOWN)
				FillTargets();
			break;
		case ID_TP:
		case ID_SPEC:
			if (int t = Target())
			{
				AdminCmd(LOWORD(w) == ID_TP ? "tp" : "spec", t);
				ShowHud(false);
			}
			break;
		case ID_SPEC_STOP:
			AdminCmd("spec", 0);
			ShowHud(false);
			break;
		case ID_SAVE:
			if (!g_inMP)
				Status(L"Salvar só funciona dentro de um jogo co-op.");
			else
			{
				Queue("cmd cofmp_save manual\n");
				Status(L"Partida salva no PC do host (aparece em Carregar no PC dele).");
				SetTimer(h, TIMER_SAVES, 1500, NULL);
			}
			break;
		case ID_LOAD:
			DoLoad();
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
// alcanca pelas saidas, mais os do Doctor), a dos policiais (c_cof_*) e os mapas da comunidade (todo o resto). Cada
// campanha aparece na ordem em que se joga (pelos capitulos e saidas dos mapas); mapas desconhecidos vao no fim.
static const char* const SIMON_MAPS[] = { "c_intro", "c_nightmare", "c_start", "c_start2", "c_apartment1",
	"c_apartment2", "c_apartment5", "c_apartment3", "c_apartment4", "c_apartmentsick", "c_basement", "c_sewer",
	"c_sewer2", "c_sewer3", "c_city", "c_city2", "c_park", "c_park2", "c_stad", "c_hamn", "c_hamn2", "c_hamn3",
	"c_college1", "c_college2", "c_college1dark", "c_park3", "c_roofs", "c_roofs2", "c_buscity2", "c_buscity",
	"c_avlopp", "c_apartment3sick", "c_apartment4sick", "c_down", "c_subway1st1", "c_subtunnel1", "c_subtunnel2",
	"c_subway2st1", "c_subway2st2", "c_subwaysick1", "c_subwaysick2", "c_subway2st3", "c_trainscene", "c_trainride",
	"c_trainride2", "c_broscene", "c_bridge", "c_forest1", "c_forest2", "c_forest3", "c_forest4", "c_forest5",
	"c_forest6", "c_forest7", "c_asylum1", "c_asylum2", "c_asylumb", "c_asylum2dark", "c_asylum1dark", "c_attic",
	"c_asylum1day", "c_forestday", "c_lake", "c_fsewer1", "c_village1", "c_village2", "c_village3", "c_ending1",
	"c_ending1b", "c_ending1c", "c_ending2", "c_ending2b", "c_ending2c", "c_ending3", "c_ending3b", "c_ending3c",
	"c_ending4", "c_ending4b", "c_ending5", "c_ending5b", "c_ending5c", "c_doc_city", "c_doc_sewer3", "c_doc_sewer2",
	"c_doc_sewer", "c_doc_basement", "c_doc_appark", "c_doc_apartment4sick", "c_doc_apartment3sick", "c_doc_ending" };
static const char* const COPS_MAPS[] = { "c_cof_village2", "c_cof_village1", "c_cof_fsewer1", "c_cof_lake",
	"c_cof_forestday", "c_cof_asylum1day", "c_cof_asylum2", "c_cof_asylum1", "c_cof_forest7", "c_cof_forest6",
	"c_cof_forest5", "c_cof_forest4", "c_cof_forest3", "c_cof_forest2", "c_cof_forest1", "c_cof_bridge",
	"c_cof_trainride2", "c_cof_trainride", "c_cof_subway2st3", "c_cof_subway2st2", "c_cof_subway2st1", "c_cof_buscity",
	"c_cof_city", "c_cof_city2", "c_cof_park", "c_cof_park2", "c_cof_stad", "c_cof_subway1st1", "c_cof_coopend",
	"c_cof_alley", "c_cof_street", "c_cof_subway1", "c_cof_subway2", "c_cof_subway3", "c_cof_gasstation",
	"c_cof_truckride", "c_cof_foresttrail1", "c_cof_hospital", "c_cof_foresttrail2", "c_cof_foresttrail3",
	"c_cof_cabin" };
// as campanhas da comunidade que vem com o jogo, cada uma na ordem das saidas
static const char* const COMMUNITY_MAPS[] = { "c_arvuti", "c_drean", "c_carnie1", "c_carnie2", "c_tazo",
	"c_aresaka", "c_nalopex", "c_theevina", "c_doc_swift", "c_collab_credits", "c_egypt1", "c_egypt2", "c_egypt3",
	"c_genpat", "c_nmare", "c_lobby", "c_garden", "c_lobbys", "c_base", "c_ls2", "c_gards", "c_ks", "c_esewer",
	"c_ec9bc", "c_ec10", "c_ec11", "c_hallo_iambulletproof", "c_hallo_nalopex", "c_hallo_darkydude", "c_hallo_carnc",
	"c_hallo_zikshadow", "c_hallo_cofbrony", "c_hallo_theevina", "c_doc_hallo_e3", "c_doc_hallo_aom_1",
	"c_doc_hallo_aom_2", "c_her1", "c_her2", "c_her3", "c_her4", "c_her5", "c_hole1", "c_hole2", "c_hole3", "c_hole4",
	"c_hole5", "c_hole_ending", "c_house1", "c_sinner", "c_house2", "c_theater", "c_ntfd_hospital", "c_ntfd_bookworld",
	"c_house3", "c_eviction", "c_house4", "c_martyr_void1", "c_martyr_void2", "c_hideout", "c_horda", "c_house_end",
	"c_endinga", "c_is_farm1", "c_is_park1", "c_is_nm1", "c_nightmare1", "c_nightmare2", "c_nightmare3",
	"c_nightmare4", "c_outofit00", "c_outofit0", "c_outofit1", "c_outofit2", "c_outofit3", "c_outofit4", "c_outofit5",
	"c_outofit6", "c_outofit7", "c_outofit8", "c_outofit9", "c_outofit10", "c_outofit11", "c_outofit12", "c_rumpel1",
	"c_rumpel2", "c_arvooti", "c_tricky", "c_sebcollab_1", "c_iambulletproof", "c_theevinas", "c_vr_nalopex1",
	"c_vr_nalopex2", "c_vr_nalopex3", "c_volcomator1", "c_volcomator2a", "c_volcomator2b", "c_volcomator2c",
	"c_volcomator3", "c_zikshadow", "c_sdkmap1", "c_sdkmap2", "c_the_stairway1", "c_the_stairway2", "c_the_stairway3",
	"c_the_stairway4", "c_the_stairway5" };
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
	std::vector<std::string> found;
	WIN32_FIND_DATAA fd;
	HANDLE fh = FindFirstFileA((g_cry + "\\maps\\c_*.bsp").c_str(), &fd);
	if (fh != INVALID_HANDLE_VALUE)
	{
		do
		{
			*strrchr(fd.cFileName, '.') = 0;
			int group = MapGroup(fd.cFileName);
			if (group && IsDlgButtonChecked(g_hud, group) == BST_CHECKED)
				found.push_back(fd.cFileName);
		} while (FindNextFileA(fh, &fd));
		FindClose(fh);
	}
	auto add = [&](const char* name) {
		for (auto it = found.begin(); it != found.end(); ++it)
			if (!_stricmp(it->c_str(), name))
			{
				SendMessageA(map, CB_ADDSTRING, 0, (LPARAM)it->c_str());
				found.erase(it);
				return;
			}
	};
	for (const char* m : SIMON_MAPS)
		add(m);
	for (const char* m : COPS_MAPS)
		add(m);
	for (const char* m : COMMUNITY_MAPS)
		add(m);
	std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) {
		return _stricmp(a.c_str(), b.c_str()) < 0;
	});
	for (auto& m : found) // c_cof_* e da comunidade que nao conhecemos
		SendMessageA(map, CB_ADDSTRING, 0, (LPARAM)m.c_str());
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
	RECT r = { 0, 0, 630, 380 };
	AdjustWindowRectEx(&r, style, FALSE, ex);
	g_hud = CreateWindowExW(ex, wc.lpszClassName, L"Cry of Fear co-op v" COFMP_VERSION, style, 0, 0, r.right - r.left,
		r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);

	const DWORD S = SS_LEFT, SC = SS_LEFT | SS_CENTERIMAGE, B = WS_TABSTOP | BS_PUSHBUTTON,
				E = WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
	Ctl(L"STATIC", L"Cry of Fear — co-op", S, 12, 10, 606, 26, 0, g_bold);

	Ctl(L"BUTTON", L"Hospedar", BS_GROUPBOX, 10, 40, 360, 156, 0);
	Ctl(L"STATIC", L"Jogadores:", SC, 22, 62, 70, 23, 0);
	HWND players = Ctl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, 95, 62, 55, 120, ID_PLAYERS);
	for (const wchar_t* n : { L"2", L"3", L"4" })
		SendMessageW(players, CB_ADDSTRING, 0, (LPARAM)n);
	SendMessageW(players, CB_SETCURSEL, 0, 0); // o mapa espera esse numero de jogadores para comecar
	Ctl(L"STATIC", L"Mapa:", SC, 162, 62, 40, 23, 0);
	HWND map = Ctl(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, 205, 62,
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

	// painel da partida em andamento: vale para quem esta dentro de um jogo co-op (host ou convidado)
	Ctl(L"BUTTON", L"Comandos", BS_GROUPBOX, 380, 40, 240, 156, 0);
	Ctl(L"BUTTON", L"Noclip", C, 392, 62, 100, 20, ID_NOCLIP);
	Ctl(L"BUTTON", L"God mode", C, 500, 62, 108, 20, ID_GOD);
	Ctl(L"STATIC", L"Jogador:", SC, 392, 92, 55, 23, 0);
	Ctl(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 450, 92, 158, 200, ID_TARGET);
	Ctl(L"BUTTON", L"Teleportar até ele", B, 392, 122, 216, 28, ID_TP);
	Ctl(L"BUTTON", L"Espectar", B, 392, 158, 104, 26, ID_SPEC);
	Ctl(L"BUTTON", L"Parar", B, 504, 158, 104, 26, ID_SPEC_STOP);

	Ctl(L"BUTTON", L"Partida salva", BS_GROUPBOX, 380, 206, 240, 100, 0);
	Ctl(L"BUTTON", L"Salvar agora", B, 392, 226, 216, 23, ID_SAVE);
	Ctl(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 392, 254, 216, 200, ID_SAVES);
	Ctl(L"BUTTON", L"Carregar (abre o servidor)", B, 392, 279, 216, 23, ID_LOAD);
	FillSaves();

	Ctl(L"STATIC", L"", S, 12, 316, 606, 36, ID_STATUS);
	Ctl(L"STATIC", L"Home abre e fecha esta janela. Os dois usam a Steam aberta.", S, 12, 356, 500, 18, 0);
	Ctl(L"STATIC", L"v" COFMP_VERSION, SS_RIGHT, 560, 356, 58, 18, 0);
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
