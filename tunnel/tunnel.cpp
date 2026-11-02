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
#include <vector>
#include "../logfile.h"

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
	virtual bool GetFriendGamePlayed(CSteamID, void* info) = 0;
};

struct FriendGameInfo_t
{
	uint64_t gameID; // os 24 bits de baixo sao o appid
	uint32_t ip;
	uint16_t port, queryPort;
	CSteamID lobby;
	uint8_t pad[8];
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

// printf que tambem vai para o cofmp_log.txt
static void Out(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	buf[sizeof(buf) - 1] = 0;
	fputs(buf, stdout);
	const char* s = buf + strspn(buf, " \n");
	if (*s)
		LogFile_Write("tunel", s);
}

// so no log
static void Note(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	buf[sizeof(buf) - 1] = 0;
	LogFile_Write("tunel", buf);
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
			Out("  Conexao P2P aceita: %s\n", Name(id));
		}
	}
	void Run(void* p, bool, uint64_t) override { Run(p); }
	int GetCallbackSizeBytes() override { return sizeof(CSteamID); }
};

class ConnectFail : public CCallbackBase
{
public:
	void Run(void* p) override;
	void Run(void* p, bool, uint64_t) override { Run(p); }
	int GetCallbackSizeBytes() override { return 12; }
};

// Pacote do jogo maior que 1200 bytes vai em pedacos sem confirmacao, com o cabecalho FD FF FF FF <id:2> <i:1> <n:1>.
// Mandar esses pela fila confiavel da Steam (como ate a v1.7) fazia eles chegarem depois dos pequenos que vieram
// atras; o netchan do jogo descarta pacote fora de ordem, e eram justamente os grandes que levavam os comandos e
// eventos (tiro, inventario...), reenviados varias vezes ate passar: o "lag" de tudo que nao e andar.
// Um pedaco perdido perde o pacote inteiro, que o netchan trata como perda comum. O 0xFFFFFFFD nunca e o comeco
// de um pacote do jogo (sequencia 0x3FFFFFFD; os sem conexao comecam com FF FF FF FF).
static const uint8_t FRAG_MAGIC[4] = { 0xFD, 0xFF, 0xFF, 0xFF };
static const uint32_t FRAG_HDR = 8, FRAG_DATA = MAX_UNRELIABLE - FRAG_HDR;

static struct
{
	uint32_t pktOut, pktIn, bytesOut, bytesIn, fragOut, fragIn, fragLost, sendFail;
} g_stats;

static void Send(CSteamID to, const char* data, int len)
{
	g_stats.pktOut++;
	g_stats.bytesOut += len;
	if ((uint32_t)len <= MAX_UNRELIABLE)
	{
		if (!g_net->SendP2PPacket(to, data, len, P2P_UNRELIABLE, CH_GAME))
			g_stats.sendFail++;
		return;
	}
	static uint16_t nextId;
	uint16_t id = nextId++;
	uint32_t count = (len + FRAG_DATA - 1) / FRAG_DATA;
	if (count > 255)
		return;
	char frag[MAX_UNRELIABLE];
	for (uint32_t i = 0; i < count; i++)
	{
		uint32_t off = i * FRAG_DATA, n = min((uint32_t)len - off, FRAG_DATA);
		memcpy(frag, FRAG_MAGIC, 4);
		memcpy(frag + 4, &id, 2);
		frag[6] = (char)i;
		frag[7] = (char)count;
		memcpy(frag + FRAG_HDR, data + off, n);
		if (!g_net->SendP2PPacket(to, frag, FRAG_HDR + n, P2P_UNRELIABLE, CH_GAME))
			g_stats.sendFail++;
		g_stats.fragOut++;
	}
}

// remonta um pacote em pedacos; um por remetente (os pedacos de um pacote vem em sequencia)
struct Assembly
{
	uint16_t id;
	uint32_t count, have;
	std::vector<std::string> parts;
};
static std::map<uint64_t, Assembly> g_assembly;

// true quando data/len e um pacote inteiro do jogo (pedaco que completou o pacote ou pacote normal)
static bool Reassemble(CSteamID from, const char*& data, uint32_t& len, std::string& whole)
{
	if (len < FRAG_HDR || memcmp(data, FRAG_MAGIC, 4))
		return true;
	g_stats.fragIn++;
	uint16_t id;
	memcpy(&id, data + 4, 2);
	uint32_t i = (uint8_t)data[6], count = (uint8_t)data[7];
	if (!count || i >= count)
		return false;
	Assembly& a = g_assembly[from.v];
	if (a.parts.empty() || a.id != id || a.count != count)
	{
		if (!a.parts.empty() && a.have < a.count)
			g_stats.fragLost++;
		a.id = id;
		a.count = count;
		a.have = 0;
		a.parts.assign(count, std::string());
	}
	if (!a.parts[i].empty())
		return false;
	a.parts[i].assign(data + FRAG_HDR, len - FRAG_HDR);
	if (++a.have < a.count)
		return false;
	whole.clear();
	for (const std::string& p : a.parts)
		whole += p;
	a.parts.clear();
	data = whole.data();
	len = (uint32_t)whole.size();
	return true;
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
// sessao (P2PSessionRequest) pode nunca chegar aqui. O host aceita por conta propria os amigos que estao no Cry of Fear.
// Sem pedido pendente, o Accept abre uma conexao para o amigo; quem nao responde fica 1 min de fora (g_quiet).
static std::map<uint64_t, DWORD> g_quiet;
static std::map<uint64_t, bool> g_greeted; // host: amigos que ja mandaram o "cofmp <versao>"

static void AcceptFriends(DWORD now)
{
	int n = g_friends->GetFriendCount(FRIEND_IMMEDIATE);
	for (int k = 0; k < n; k++)
	{
		CSteamID id = g_friends->GetFriendByIndex(k, FRIEND_IMMEDIATE);
		FriendGameInfo_t game = {};
		if (g_greeted[id.v] || !g_friends->GetFriendGamePlayed(id, &game) || (game.gameID & 0xFFFFFF) != 223710)
			continue;
		auto q = g_quiet.find(id.v);
		if (q == g_quiet.end() || (int)(now - q->second) >= 0)
			g_net->AcceptP2PSessionWithUser(id);
	}
}

void ConnectFail::Run(void* p)
{
	CSteamID id = *(CSteamID*)p;
	if (g_host && !g_greeted[id.v]) // conexao que o AcceptFriends abriu para um amigo sem o tunel: ignora por 1 min
	{
		g_quiet[id.v] = timeGetTime() + 60000;
		return;
	}
	if (!g_host && id.v != g_peer.v)
		return;
	Out("  Falha na conexao P2P com %s (erro %d). Confira o codigo e se o outro lado esta com o tunel aberto.\n",
		Name(id), ((uint8_t*)p)[8]);
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
	Out("%s", prompt);
	std::string s;
	std::getline(std::cin, s);
	size_t a = s.find_first_not_of(" \t\r\""), b = s.find_last_not_of(" \t\r\"");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static int Fail(const char* msg)
{
	Out("\n  ERRO: %s\n\n  Aperte Enter para sair.", msg);
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
	Out("==============================================\n");
	Out("  Cry of Fear co-op - tunel pela Steam (v%s)\n", COFMP_VERSION);
	Out("==============================================\n\n");

	// a Steam identifica o app pelo steam_appid.txt da pasta atual
	char dir[MAX_PATH];
	GetModuleFileNameA(NULL, dir, MAX_PATH);
	*strrchr(dir, '\\') = 0;
	SetCurrentDirectoryA(dir);
	{
		// instalado: cryoffear\cl_dlls\cofmp_tunel -> log em cryoffear; rodando de outra pasta: log nela
		char logDir[MAX_PATH];
		strcpy(logDir, dir);
		size_t L = strlen(logDir), S = strlen("\\cl_dlls\\cofmp_tunel");
		if (L > S && !_stricmp(logDir + L - S, "\\cl_dlls\\cofmp_tunel"))
			logDir[L - S] = 0;
		LogFile_Open(logDir);
	}
	Note("---- tunel v%s aberto: %s", COFMP_VERSION, GetCommandLineA());
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
		Out("  1) Hospedar (quem abriu o servidor no jogo)\n");
		Out("  2) Entrar no jogo de um amigo\n\n");
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
		Out("  Tunel aberto como %s.\n\n", g_friends->GetPersonaName());
		Out("  Codigo para o seu amigo:  %s\n", buf);
		Out("  (ja foi copiado; mande para ele pelo chat da Steam)\n\n");
		Out("  Deixe esta janela aberta enquanto jogam. Servidor local: 127.0.0.1:%d\n\n", gamePort);
	}
	else
	{
		listenSock = UdpSocket(localPort);
		if (listenSock == INVALID_SOCKET)
			return Fail("a porta local ja esta em uso (outro tunel aberto?).");
		DeleteFileA("conectado.txt"); // a janela do jogo (Home) espera este arquivo para mandar o connect
		Out("  Conectando ao host %s pela Steam...\n\n", Name(g_peer));
		Out("  Deixe esta janela aberta enquanto jogam.\n\n");
	}

	static char buf[65536];
	std::string whole;
	DWORD lastCb = 0, lastStatus = 0, lastHello = 0, lastAccept = 0, lastStats = 0, start = timeGetTime();
	bool wasRelay = false, announced = false, hostAnswered = false, warned = false;

	for (;;)
	{
		bool busy = false;
		DWORD now = timeGetTime();
		if (now - lastCb >= 10)
		{
			pRunCallbacks();
			lastCb = now;
		}
		if (g_host && now - lastAccept >= 2000)
		{
			AcceptFriends(now);
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
				if (!g_greeted[from.v])
				{
					g_greeted[from.v] = true;
					Out("  %s abriu o tunel para entrar (v%s).\n", Name(from), ver.c_str());
					if (ver != COFMP_VERSION)
						Out("  AVISO: versoes diferentes (voce: v%s). Os dois devem rodar o ATUALIZAR.bat.\n", COFMP_VERSION);
				}
			}
			else if (from.v == g_peer.v && !hostAnswered)
			{
				hostAnswered = true;
				Out("  O tunel do host respondeu (v%s). Entrando no jogo...\n", ver.c_str());
				if (ver != COFMP_VERSION)
					Out("  AVISO: versoes diferentes (voce: v%s). Os dois devem rodar o ATUALIZAR.bat.\n", COFMP_VERSION);
				Out("  Se o jogo nao entrar sozinho, digite no console:  connect 127.0.0.1:%d\n\n", localPort);
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
				Out("  O tunel do host ainda nao respondeu. Confira:\n");
				Out("   - o codigo e o do host e ele clicou em \"Criar servidor\" (o tunel dele tem que estar aberto);\n");
				Out("   - os dois estao na mesma versao (esta e a v%s; o host ve a dele na janela do tunel);\n", COFMP_VERSION);
				Out("   - voces sao amigos na Steam.\n  Continuo tentando...\n\n");
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
			const char* pkt = buf;
			if (!Reassemble(from, pkt, got, whole))
				continue;
			g_stats.pktIn++;
			g_stats.bytesIn += got;
			if (g_host)
			{
				auto it = peers.find(from.v);
				if (it == peers.end())
				{
					Peer p = { UdpSocket(0), now };
					if (p.sock == INVALID_SOCKET)
						continue;
					it = peers.emplace(from.v, p).first;
					Out("  %s entrou pelo tunel.\n", Name(from));
				}
				it->second.last = now;
				sendto(it->second.sock, pkt, got, 0, (sockaddr*)&game, sizeof(game));
			}
			else if (from.v == g_peer.v && haveGame)
				sendto(listenSock, pkt, got, 0, (sockaddr*)&game, sizeof(game));
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
					Out("  %s saiu do tunel.\n", Name(CSteamID{ it->first }));
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
						Out("  Conectado ao host (%s).\n", st.usingRelay ? "pelos servidores da Steam" : "direto");
					announced = true;
					wasRelay = !!st.usingRelay;
				}
			}
		}
		// a cada 10 s, no log: trafego e estado da conexao com cada um (fila da Steam cheia = conexao lenta)
		if (now - lastStats >= 10000)
		{
			lastStats = now;
			Note("10s: pacotes env %u (%u B, %u pedacos, %u falhas) rec %u (%u B, %u pedacos, %u perdidos)",
				g_stats.pktOut, g_stats.bytesOut, g_stats.fragOut, g_stats.sendFail, g_stats.pktIn, g_stats.bytesIn,
				g_stats.fragIn, g_stats.fragLost);
			memset(&g_stats, 0, sizeof(g_stats));
			auto state = [&](CSteamID id) {
				P2PSessionState_t st = {};
				if (g_net->GetP2PSessionState(id, &st))
					Note("  %s: ativa %d conectando %d erro %d %s, fila %d B / %d pacotes", Name(id), st.connectionActive,
						st.connecting, st.sessionError, st.usingRelay ? "pelo relay da Steam" : "direto", st.bytesQueued,
						st.packetsQueued);
			};
			if (g_host)
				for (auto& p : peers)
					state(CSteamID{ p.first });
			else
				state(g_peer);
		}
		if (!busy)
			Sleep(1);
	}
}
