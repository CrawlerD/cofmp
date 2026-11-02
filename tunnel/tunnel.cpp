// Tunel UDP pela rede P2P da Steam, para jogar sem abrir porta. Se identifica como o proprio Cry of Fear
// (appid 223710): a Steam passou a recusar o SteamAPI_Init com o appid 480 (Spacewar).
// host: recebe pacotes P2P dos amigos e entrega no servidor local (127.0.0.1:27015), um socket por amigo.
// join: escuta 127.0.0.1:27020; o jogo faz "connect 127.0.0.1:27020" e os pacotes vao para o host pela Steam.
// Usa o steam_api.dll antigo (interfaces SteamNetworking005 / SteamUser016 / SteamFriends013) carregado em runtime.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <map>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")

struct CSteamID { uint64_t v; };

class ISteamUser
{
public:
	virtual int GetHSteamUser() = 0;
	virtual bool BLoggedOn() = 0;
	virtual CSteamID GetSteamID() = 0;
};

class ISteamFriends
{
public:
	virtual const char* GetPersonaName() = 0;
	virtual void SetPersonaName_(const char*) = 0;
	virtual int GetPersonaState() = 0;
	virtual int GetFriendCount(int) = 0;
	virtual CSteamID GetFriendByIndex(int, int) = 0;
	virtual int GetFriendRelationship(CSteamID) = 0;
	virtual int GetFriendPersonaState(CSteamID) = 0;
	virtual const char* GetFriendPersonaName(CSteamID) = 0;
};

struct P2PSessionState_t
{
	uint8_t connectionActive, connecting, sessionError, usingRelay;
	int32_t bytesQueued, packetsQueued;
	uint32_t remoteIP;
	uint16_t remotePort;
	uint8_t pad[16];
};

class ISteamNetworking
{
public:
	virtual bool SendP2PPacket(CSteamID to, const void* data, uint32_t len, int sendType, int channel) = 0;
	virtual bool IsP2PPacketAvailable(uint32_t* size, int channel) = 0;
	virtual bool ReadP2PPacket(void* dest, uint32_t cap, uint32_t* size, CSteamID* from, int channel) = 0;
	virtual bool AcceptP2PSessionWithUser(CSteamID) = 0;
	virtual bool CloseP2PSessionWithUser(CSteamID) = 0;
	virtual bool CloseP2PChannelWithUser(CSteamID, int) = 0;
	virtual bool GetP2PSessionState(CSteamID, P2PSessionState_t*) = 0;
	virtual bool AllowP2PPacketRelay(bool) = 0;
};

// mesmo layout do CCallbackBase do SDK (a ordem dos virtuais tem que bater)
class CCallbackBase
{
public:
	virtual void Run(void* param) = 0;
	virtual void Run(void* param, bool ioFailure, uint64_t call) = 0;
	virtual int GetCallbackSizeBytes() = 0;
	uint8_t m_nCallbackFlags = 0;
	int m_iCallback = 0;
};

enum { P2P_UNRELIABLE = 0, P2P_RELIABLE = 2, CB_SESSION_REQUEST = 1202, CB_CONNECT_FAIL = 1203 };
enum { CH_GAME = 0, CH_HELLO = 1 }; // canal 1: "cofmp <versao>" de ida e volta, para saber que o tunel do outro lado responde
enum { FRIEND_IMMEDIATE = 4 };
static const char* APPID = "223710"; // Cry of Fear
static const uint32_t MAX_UNRELIABLE = 1200; // acima disso a Steam so aceita envio confiavel

#ifndef COFMP_VERSION
#define COFMP_VERSION "?"
#endif

static bool (__cdecl* pInit)();
static void (__cdecl* pRunCallbacks)();
static void (__cdecl* pRegisterCallback)(CCallbackBase*, int);
static ISteamUser* (__cdecl* pUser)();
static ISteamNetworking* (__cdecl* pNet)();
static ISteamFriends* (__cdecl* pFriends)();

static ISteamNetworking* g_net;
static ISteamFriends* g_friends;
static bool g_host;
static CSteamID g_peer; // join: o host

static const char* Name(CSteamID id)
{
	const char* n = g_friends ? g_friends->GetFriendPersonaName(id) : NULL;
	return n && *n ? n : "?";
}

class SessionRequest : public CCallbackBase
{
public:
	void Run(void* p) override
	{
		CSteamID id = *(CSteamID*)p;
		// host aceita qualquer um que tenha o codigo; join so aceita o proprio host
		if (g_host || id.v == g_peer.v)
		{
			g_net->AcceptP2PSessionWithUser(id);
			printf("  Conexao P2P aceita: %s\n", Name(id));
		}
	}
	void Run(void* p, bool, uint64_t) override { Run(p); }
	int GetCallbackSizeBytes() override { return sizeof(CSteamID); }
};

class ConnectFail : public CCallbackBase
{
public:
	void Run(void* p) override
	{
		printf("  Falha na conexao P2P com %s (erro %d). Confira o codigo e se o outro lado esta com o tunel aberto.\n",
			Name(*(CSteamID*)p), ((uint8_t*)p)[8]);
	}
	void Run(void* p, bool, uint64_t) override { Run(p); }
	int GetCallbackSizeBytes() override { return 12; }
};

static void Send(CSteamID to, const char* data, int len)
{
	g_net->SendP2PPacket(to, data, len, (uint32_t)len <= MAX_UNRELIABLE ? P2P_UNRELIABLE : P2P_RELIABLE, CH_GAME);
}

static void SendHello(CSteamID to)
{
	static const char hello[] = "cofmp " COFMP_VERSION;
	g_net->SendP2PPacket(to, hello, sizeof(hello), P2P_RELIABLE, CH_HELLO);
}

// versao que veio no "cofmp <versao>" do outro lado
static std::string HelloVersion(const char* data, uint32_t len)
{
	std::string s(data, strnlen(data, len));
	return s.compare(0, 6, "cofmp ") ? "" : s.substr(6);
}

// O jogo (cof.exe) tambem roda como appid 223710 e consome os callbacks da Steam do processo dele, entao o pedido de
// sessao (P2PSessionRequest) pode nunca chegar aqui. O host aceita por conta propria quem estiver online na lista de amigos.
static void AcceptFriends()
{
	int n = g_friends->GetFriendCount(FRIEND_IMMEDIATE);
	for (int k = 0; k < n; k++)
	{
		CSteamID id = g_friends->GetFriendByIndex(k, FRIEND_IMMEDIATE);
		if (g_friends->GetFriendPersonaState(id) != 0)
			g_net->AcceptP2PSessionWithUser(id);
	}
}

static SOCKET UdpSocket(uint16_t port)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (s == INVALID_SOCKET || bind(s, (sockaddr*)&a, sizeof(a)) != 0)
	{
		if (s != INVALID_SOCKET)
			closesocket(s);
		return INVALID_SOCKET;
	}
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);
	// sem isso um ICMP "porta fechada" faz o recvfrom falhar com WSAECONNRESET para sempre
	BOOL off = FALSE;
	DWORD ret;
	WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), NULL, 0, &ret, NULL, NULL);
	return s;
}

static void CopyToClipboard(const std::string& s)
{
	if (!OpenClipboard(NULL))
		return;
	EmptyClipboard();
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
	memcpy(GlobalLock(h), s.c_str(), s.size() + 1);
	GlobalUnlock(h);
	SetClipboardData(CF_TEXT, h);
	CloseClipboard();
}

static std::string Ask(const char* prompt)
{
	printf("%s", prompt);
	std::string s;
	std::getline(std::cin, s);
	size_t a = s.find_first_not_of(" \t\r\""), b = s.find_last_not_of(" \t\r\"");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static int Fail(const char* msg)
{
	printf("\n  ERRO: %s\n\n  Aperte Enter para sair.", msg);
	std::string s;
	std::getline(std::cin, s);
	return 1;
}

struct Peer
{
	SOCKET sock;
	DWORD last;
};

int main(int argc, char** argv)
{
	SetConsoleOutputCP(CP_UTF8);
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("==============================================\n");
	printf("  Cry of Fear co-op - tunel pela Steam (v%s)\n", COFMP_VERSION);
	printf("==============================================\n\n");

	// a Steam identifica o app pelo steam_appid.txt da pasta atual
	char dir[MAX_PATH];
	GetModuleFileNameA(NULL, dir, MAX_PATH);
	*strrchr(dir, '\\') = 0;
	SetCurrentDirectoryA(dir);
	FILE* f = fopen("steam_appid.txt", "w");
	if (f)
	{
		fputs(APPID, f);
		fclose(f);
	}
	SetEnvironmentVariableA("SteamAppId", APPID);
	SetEnvironmentVariableA("SteamGameId", APPID);

	std::string mode = argc > 1 ? argv[1] : "";
	if (mode.empty())
	{
		printf("  1) Hospedar (quem abriu o servidor no jogo)\n");
		printf("  2) Entrar no jogo de um amigo\n\n");
		mode = Ask("  Escolha: ") == "1" ? "host" : "join";
	}
	g_host = mode == "host";
	uint16_t gamePort = 27015, localPort = 27020;
	std::string code;
	if (g_host)
	{
		if (argc > 2)
			gamePort = (uint16_t)atoi(argv[2]);
	}
	else
	{
		code = argc > 2 ? argv[2] : Ask("  Cole o codigo que o host te passou: ");
		g_peer.v = _strtoui64(code.c_str(), NULL, 10);
		if (argc > 3)
			localPort = (uint16_t)atoi(argv[3]);
		if ((g_peer.v >> 52) != 0x011) // SteamID64 de conta individual comeca com 0x0110000100...
			return Fail("codigo invalido. Ele e o numero grande que aparece no tunel do host.");
	}

	HMODULE api = LoadLibraryA("steam_api.dll");
	if (!api)
		return Fail("steam_api.dll nao esta na mesma pasta do tunel.");
	pInit = (decltype(pInit))GetProcAddress(api, "SteamAPI_Init");
	pRunCallbacks = (decltype(pRunCallbacks))GetProcAddress(api, "SteamAPI_RunCallbacks");
	pRegisterCallback = (decltype(pRegisterCallback))GetProcAddress(api, "SteamAPI_RegisterCallback");
	pUser = (decltype(pUser))GetProcAddress(api, "SteamUser");
	pNet = (decltype(pNet))GetProcAddress(api, "SteamNetworking");
	pFriends = (decltype(pFriends))GetProcAddress(api, "SteamFriends");
	if (!pInit || !pRunCallbacks || !pRegisterCallback || !pUser || !pNet || !pFriends)
		return Fail("steam_api.dll incompativel.");
	if (!pInit())
		return Fail("nao consegui falar com a Steam. Ela esta aberta e logada?");
	g_net = pNet();
	g_friends = pFriends();
	if (!g_net)
		return Fail("a Steam nao entregou a interface de rede.");
	g_net->AllowP2PPacketRelay(true);

	static SessionRequest onRequest;
	static ConnectFail onFail;
	pRegisterCallback(&onRequest, CB_SESSION_REQUEST);
	pRegisterCallback(&onFail, CB_CONNECT_FAIL);

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	timeBeginPeriod(1);

	sockaddr_in game = {};
	game.sin_family = AF_INET;
	game.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	std::map<uint64_t, Peer> peers; // host: um socket por amigo
	SOCKET listenSock = INVALID_SOCKET; // join: onde o jogo conecta
	bool haveGame = false;

	if (g_host)
	{
		CSteamID me = pUser()->GetSteamID();
		char buf[32];
		sprintf(buf, "%llu", me.v);
		CopyToClipboard(buf);
		FILE* cf = fopen("codigo.txt", "w"); // lido pela janela do jogo (tecla Home)
		if (cf)
		{
			fputs(buf, cf);
			fclose(cf);
		}
		game.sin_port = htons(gamePort);
		printf("  Tunel aberto como %s.\n\n", g_friends->GetPersonaName());
		printf("  Codigo para o seu amigo:  %s\n", buf);
		printf("  (ja foi copiado; mande para ele pelo chat da Steam)\n\n");
		printf("  Deixe esta janela aberta enquanto jogam. Servidor local: 127.0.0.1:%d\n\n", gamePort);
	}
	else
	{
		listenSock = UdpSocket(localPort);
		if (listenSock == INVALID_SOCKET)
			return Fail("a porta local ja esta em uso (outro tunel aberto?).");
		DeleteFileA("conectado.txt"); // a janela do jogo (Home) espera este arquivo para mandar o connect
		printf("  Conectando ao host %s pela Steam...\n\n", Name(g_peer));
		printf("  Deixe esta janela aberta enquanto jogam.\n\n");
	}

	static char buf[65536];
	DWORD lastCb = 0, lastStatus = 0, lastHello = 0, lastAccept = 0, start = timeGetTime();
	bool wasRelay = false, announced = false, hostAnswered = false, warned = false;
	std::map<uint64_t, bool> greeted; // host: amigos que ja mandaram o "cofmp <versao>"
	for (;;)
	{
		bool busy = false;
		DWORD now = timeGetTime();
		if (now - lastCb >= 10)
		{
			pRunCallbacks();
			lastCb = now;
		}
		if (g_host && now - lastAccept >= 1000)
		{
			AcceptFriends();
			lastAccept = now;
		}

		// canal do "cofmp <versao>": o join manda ate o host responder; o host responde a cada um
		uint32_t size;
		while (g_net->IsP2PPacketAvailable(&size, CH_HELLO))
		{
			CSteamID from;
			uint32_t got = 0;
			if (!g_net->ReadP2PPacket(buf, sizeof(buf), &got, &from, CH_HELLO))
				break;
			std::string ver = HelloVersion(buf, got);
			if (g_host)
			{
				g_net->AcceptP2PSessionWithUser(from);
				SendHello(from);
				if (!greeted[from.v])
				{
					greeted[from.v] = true;
					printf("  %s abriu o tunel para entrar (v%s).\n", Name(from), ver.c_str());
					if (ver != COFMP_VERSION)
						printf("  AVISO: versoes diferentes (voce: v%s). Os dois devem rodar o ATUALIZAR.bat.\n", COFMP_VERSION);
				}
			}
			else if (from.v == g_peer.v && !hostAnswered)
			{
				hostAnswered = true;
				printf("  O tunel do host respondeu (v%s). Entrando no jogo...\n", ver.c_str());
				if (ver != COFMP_VERSION)
					printf("  AVISO: versoes diferentes (voce: v%s). Os dois devem rodar o ATUALIZAR.bat.\n", COFMP_VERSION);
				printf("  Se o jogo nao entrar sozinho, digite no console:  connect 127.0.0.1:%d\n\n", localPort);
				FILE* cf = fopen("conectado.txt", "w");
				if (cf)
					fclose(cf);
			}
		}
		if (!g_host && !hostAnswered && now - lastHello >= 1000)
		{
			SendHello(g_peer);
			lastHello = now;
			if (!warned && now - start > 20000)
			{
				warned = true;
				printf("  O tunel do host ainda nao respondeu. Confira:\n");
				printf("   - o codigo e o do host e ele clicou em \"Criar servidor\" (o tunel dele tem que estar aberto);\n");
				printf("   - os dois estao na mesma versao (esta e a v%s; o host ve a dele na janela do tunel);\n", COFMP_VERSION);
				printf("   - voces sao amigos na Steam.\n  Continuo tentando...\n\n");
			}
		}

		// Steam -> jogo
		while (g_net->IsP2PPacketAvailable(&size, CH_GAME))
		{
			CSteamID from;
			uint32_t got = 0;
			if (!g_net->ReadP2PPacket(buf, sizeof(buf), &got, &from, CH_GAME))
				break;
			busy = true;
			if (g_host)
			{
				auto it = peers.find(from.v);
				if (it == peers.end())
				{
					Peer p = { UdpSocket(0), now };
					if (p.sock == INVALID_SOCKET)
						continue;
					it = peers.emplace(from.v, p).first;
					printf("  %s entrou pelo tunel.\n", Name(from));
				}
				it->second.last = now;
				sendto(it->second.sock, buf, got, 0, (sockaddr*)&game, sizeof(game));
			}
			else if (from.v == g_peer.v && haveGame)
				sendto(listenSock, buf, got, 0, (sockaddr*)&game, sizeof(game));
		}

		// jogo -> Steam
		if (g_host)
		{
			for (auto it = peers.begin(); it != peers.end();)
			{
				int n;
				while ((n = recv(it->second.sock, buf, sizeof(buf), 0)) > 0)
				{
					Send(CSteamID{ it->first }, buf, n);
					busy = true;
				}
				if (now - it->second.last > 60000) // 1 min sem nada: o amigo saiu
				{
					printf("  %s saiu do tunel.\n", Name(CSteamID{ it->first }));
					closesocket(it->second.sock);
					g_net->CloseP2PSessionWithUser(CSteamID{ it->first });
					it = peers.erase(it);
				}
				else
					++it;
			}
		}
		else
		{
			sockaddr_in from;
			int fl = sizeof(from), n;
			while ((n = recvfrom(listenSock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl)) > 0)
			{
				game = from; // responde para onde o jogo mandou
				haveGame = true;
				Send(g_peer, buf, n);
				busy = true;
				fl = sizeof(from);
			}
			if (now - lastStatus > 2000)
			{
				lastStatus = now;
				P2PSessionState_t st = {};
				if (g_net->GetP2PSessionState(g_peer, &st) && st.connectionActive)
				{
					if (!announced || wasRelay != !!st.usingRelay)
						printf("  Conectado ao host (%s).\n", st.usingRelay ? "pelos servidores da Steam" : "direto");
					announced = true;
					wasRelay = !!st.usingRelay;
				}
			}
		}
		if (!busy)
			Sleep(1);
	}
}
