// cofmp.dll - proxy do server DLL do Cry of Fear (hl.dll) para jogar a campanha em co-op.
//
// O engine carrega cofmp.dll (liblist.gam -> gamedll). Todos os exports de entidades sao
// encaminhados para hl.dll via cofmp.def; aqui ficam apenas GiveFnptrsToDll/GetEntityAPI(2),
// os hooks de DLL_FUNCTIONS e o detour de CChangeLevel::ChangeLevelNow.
//
// v1: sincronizacao de troca de mapa
//  - trigger_changelevel de SP funciona em MP (o original sai cedo se IsDeathmatch()).
//  - Saida por toque: espera todos os jogadores vivos entrarem no volume (cofmp_exit_wait).
//  - Saida por "use" (portas/botoes): troca na hora para todos.
//  - Cada jogador chega no mapa novo na mesma posicao relativa ao info_landmark.

#include "extdll.h"
#include <intrin.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include "exports_fwd.h" // gerado por gen_def.py
#include "hud.h"
#include "logfile.h"

#ifndef FL_GODMODE
#define FL_GODMODE (1 << 6)
#endif
#ifndef DAMAGE_NO
#define DAMAGE_NO 0
#endif

// Layout de entvars_t do Cry of Fear (sdk_cof/progdefs.h); offsets lidos de gEntvarsDescription no hl.dll.
static_assert(offsetof(entvars_t, health) == 0x164, "entvars_t");
static_assert(offsetof(entvars_t, flags) == 0x1a8, "entvars_t");
static_assert(offsetof(entvars_t, dmgtime) == 0x1e8, "entvars_t");
static_assert(offsetof(entvars_t, iuser1) == 0x248, "entvars_t");

#define STRING(ofs) ((const char*)(gpGlobals->pStringBase + (unsigned int)(ofs)))

static enginefuncs_t* g_eng;
static globalvars_t* gpGlobals;
static HMODULE g_hl;
static DLL_FUNCTIONS g_orig;

// ---------------------------------------------------------------- cvars

static cvar_t cv_enable = { (char*)"cofmp_enable", (char*)"1", FCVAR_SERVER };
static cvar_t cv_exit_wait = { (char*)"cofmp_exit_wait", (char*)"1", FCVAR_SERVER };
static cvar_t cv_exit_timeout = { (char*)"cofmp_exit_timeout", (char*)"0", FCVAR_SERVER };
static cvar_t cv_keep_inv = { (char*)"cofmp_keep_inventory", (char*)"1", FCVAR_SERVER };
static cvar_t cv_share_items = { (char*)"cofmp_share_items", (char*)"1", FCVAR_SERVER };
static cvar_t cv_respawn_time = { (char*)"cofmp_respawn_time", (char*)"30", FCVAR_SERVER };
static cvar_t cv_model = { (char*)"cofmp_model", (char*)"simon", FCVAR_SERVER };
static cvar_t cv_start_wait = { (char*)"cofmp_start_wait", (char*)"1", FCVAR_SERVER };
static cvar_t cv_start_timeout = { (char*)"cofmp_start_timeout", (char*)"90", FCVAR_SERVER };
static cvar_t cv_cutscene_wait = { (char*)"cofmp_cutscene_wait", (char*)"1", FCVAR_SERVER };

// ---------------------------------------------------------------- util

static double Plat_Time()
{
	return GetTickCount64() / 1000.0;
}

static void Log(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	buf[sizeof(buf) - 1] = 0;
	g_eng->pfnServerPrint("[cofmp] ");
	g_eng->pfnServerPrint(buf);
	g_eng->pfnServerPrint("\n");
	LogFile_Write("jogo", buf);
}

// so no cofmp_log.txt (coisas frequentes demais para o console)
static void Note(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	buf[sizeof(buf) - 1] = 0;
	LogFile_Write("jogo", buf);
}

static bool IsNull(edict_t* e)
{
	return !e || e->free || g_eng->pfnIndexOfEdict(e) == 0;
}

static bool IsPlayer(edict_t* e)
{
	return !IsNull(e) && (e->v.flags & FL_CLIENT) && e->pvPrivateData && e->v.iuser1 == 0;
}

static bool IsAlive(edict_t* e)
{
	return e->v.deadflag == DEAD_NO && e->v.health > 0;
}

static edict_t* PlayerAt(int i)
{
	edict_t* e = g_eng->pfnPEntityOfEntIndex(i);
	return IsPlayer(e) ? e : NULL;
}

static bool Overlaps(edict_t* a, edict_t* b)
{
	for (int k = 0; k < 3; k++)
	{
		if (a->v.absmin[k] > b->v.absmax[k] + 1 || a->v.absmax[k] < b->v.absmin[k] - 1)
			return false;
	}
	return true;
}

static edict_t* FindLandmark(const std::string& name)
{
	if (name.empty())
		return NULL;
	edict_t* e = NULL;
	for (;;)
	{
		e = g_eng->pfnFindEntityByString(e, "targetname", name.c_str());
		if (IsNull(e))
			return NULL;
		if (!_stricmp(STRING(e->v.classname), "info_landmark"))
			return e;
	}
}

static bool HullFree(const Vector& pos, edict_t* skip)
{
	TraceResult tr;
	g_eng->pfnTraceHull(pos, pos, 0, 1 /* human_hull */, skip, &tr);
	return !tr.fStartSolid && !tr.fAllSolid;
}

// Procura um ponto livre perto de pos (anel de deslocamentos); false se nao achar.
static bool FindFreeSpot(Vector& pos, edict_t* player)
{
	if (HullFree(pos, player))
		return true;
	static const float radii[] = { 36, 72, 108 };
	for (float r : radii)
	{
		for (int a = 0; a < 8; a++)
		{
			float ang = a * 0.78539816f;
			for (float dz = 0; dz <= 36; dz += 18)
			{
				Vector p = pos + Vector(cosf(ang) * r, sinf(ang) * r, dz);
				if (HullFree(p, player))
				{
					pos = p;
					return true;
				}
			}
		}
	}
	return false;
}

// ---------------------------------------------------------------- estado

struct ChangeInfo
{
	std::string map, landmark;
};
static std::unordered_map<int, ChangeInfo> g_changelevels; // por indice de edict, mapa atual

static struct
{
	bool active;
	int ent;
	int who; // primeiro jogador que chegou na saida
	float start;
	float nextMsg;
} g_gather;

static bool g_changing; // ja pedimos changelevel neste mapa
static void StopSkip(const char* why);
static bool g_tracing; // ha entidades de um inventorydrop sendo seguidas (rastreio de itens)
static void TraceCheck(const char* during);

static struct
{
	bool pending; // transicao em andamento para "map"
	std::string map, landmark;
	Vector offset[33];
	Vector angles[33];
	bool has[33];
	// quem passou pela saida: se o ponto de um jogador nao servir, ele chega ao lado deste
	bool hasAnchor;
	Vector anchorOffset, anchorAngles;
} g_transit;

static struct
{
	bool active;
	Vector landmark;
	float deadline;
	bool place[33];
	float seen[33];
} g_arrive;

static bool g_wasAlive[33];
static float g_unstickAt[33];

// ---------------------------------------------------------------- inventario entre mapas
//
// Em MP o engine nao leva o jogador junto na troca de mapa: ele renasce vazio. Antes do
// changelevel copiamos armas/municao de cada jogador vivo e, quando ele nasce no mapa novo,
// devolvemos tudo com o proprio CBasePlayer::GiveNamedItem do jogo.

// Offsets de CBasePlayer / CBasePlayerItem (tabelas de save/restore do hl.dll)
enum
{
	OFS_PLAYER_ITEMS = 0x2514, // CBasePlayerItem* m_rgpPlayerItems[6]
	OFS_ACTIVE_ITEM = 0x252c,  // CBasePlayerItem* m_pActiveItem
	OFS_AMMO = 0x2540,         // int m_rgAmmo[32]
	OFS_INV_SLOT = 0x21fc,     // string_t inventorySlot[7]
	OFS_INV_MODEL = 0x2218,    // string_t inventorySlotModels[7]
	OFS_INV_CLASS = 0x2234,    // string_t inventorySlotClassname[7]
	OFS_FLASH_BATTERY = 0x22a8,
	OFS_GOOD_POINTS = 0x5f0,
	OFS_ITEM_NEXT = 0x164, // CBasePlayerItem* m_pNext
	OFS_ITEM_ID = 0x168,
	OFS_ITEM_CLIP = 0x1e4, // CBasePlayerWeapon::m_iClip
};
static const int AMMO_SLOTS = 32; // m_rgAmmo

template <class T> static T& Field(void* obj, int ofs)
{
	return *(T*)((char*)obj + ofs);
}

static entvars_t* PevOf(void* obj)
{
	return obj ? Field<entvars_t*>(obj, 4) : NULL;
}

// Percorre as armas do jogador (6 listas ligadas por m_pNext).
template <class F> static void ForEachItem(void* player, F fn)
{
	for (int s = 0; s < 6; s++)
	{
		int guard = 0;
		for (void* it = Field<void*>(player, OFS_PLAYER_ITEMS + s * 4); it && guard < 64; guard++)
		{
			void* next = Field<void*>(it, OFS_ITEM_NEXT);
			fn(it);
			it = next;
		}
	}
}

static void* FindItem(void* player, const char* cls)
{
	void* found = NULL;
	ForEachItem(player, [&](void* it) {
		entvars_t* pev = PevOf(it);
		if (!found && pev && !strcmp(STRING(pev->classname), cls))
			found = it;
	});
	return found;
}

typedef void(__thiscall* GiveNamedItem_t)(void* player, const char* name, bool a2, bool force);
static GiveNamedItem_t g_GiveNamedItem;
static const DWORD RVA_GiveNamedItem = 0xe0a80;
static const BYTE SIG_GiveNamedItem[] = { 0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08, 0x57, 0x8B, 0xF9, 0x85, 0xDB };

// CBasePlayer::SelectLastItem(bool force): troca m_pActiveItem por m_pLastItem (o "lastinv" do jogo).
typedef void(__thiscall* SelectLastItem_t)(void* player, bool force);
static SelectLastItem_t g_SelectLastItem;
static const DWORD RVA_SelectLastItem = 0xf1c20;
static const BYTE SIG_SelectLastItem[] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x34, 0x25, 0x00, 0x00 };
enum
{
	OFS_LAST_ITEM = 0x2534, // CBasePlayerItem* m_pLastItem
};

// CBasePlayer::AddToInventory(string_t file, string_t model, string_t classname): poe o item no primeiro
// slot livre e avisa o cliente; nao duplica item que o jogador ja tem. Retorna 0 se o inventario esta cheio.
typedef int(__thiscall* AddToInventory_t)(void* player, int file, int model, int classname);
static AddToInventory_t g_AddToInventory;
static const DWORD RVA_AddToInventory = 0xdad40;
static const BYTE SIG_AddToInventory[] = { 0x55, 0x8B, 0xEC, 0x83, 0x7D, 0x08, 0x00, 0x53, 0x8B, 0xD9, 0x75, 0x17 };

// CBasePlayer::RemoveInventoryItem(string_t file, bool all): tira o primeiro slot com esse arquivo
// (all = esvazia o inventario inteiro) e avisa o cliente.
typedef void(__thiscall* RemoveInventoryItem_t)(void* player, int file, bool all);
static RemoveInventoryItem_t g_RemoveInventoryItem;
static const DWORD RVA_RemoveInventoryItem = 0xdda70;
static const char SIG_RemoveInventoryItem[] = "\x55\x8B\xEC\x51\xA1????\x53\x56\x57"; // ? = endereco relocado

static bool SigMatch(const BYTE* p, const char* sig, size_t n)
{
	for (size_t k = 0; k < n; k++)
		if (sig[k] != '?' && (BYTE)sig[k] != p[k])
			return false;
	return true;
}

static bool IsWeaponFile(const char* file)
{
	return strstr(file, "/weapons/") != NULL;
}

// Itens do inventario do CoF que nao sao armas: chaves, fitas, corda, bateria...
static std::vector<std::string> ItemsOf(void* pl)
{
	std::vector<std::string> out;
	for (int k = 0; k < 7; k++)
	{
		int str = Field<int>(pl, OFS_INV_SLOT + k * 4);
		if (str && *STRING(str) && !IsWeaponFile(STRING(str)))
			out.push_back(STRING(str));
	}
	return out;
}

static bool Has(const std::vector<std::string>& v, const std::string& x)
{
	return std::find(v.begin(), v.end(), x) != v.end();
}

// pfnAllocString nunca devolve memoria ate a troca de mapa: guarda as strings ja alocadas
static std::unordered_map<std::string, int> g_strings;

static int EngString(const std::string& s)
{
	auto it = g_strings.find(s);
	if (it != g_strings.end())
		return it->second;
	int v = g_eng->pfnAllocString(s.c_str());
	g_strings[s] = v;
	return v;
}

static bool GiveInvItem(void* pl, const std::string& file)
{
	return g_AddToInventory(pl, EngString(file), EngString(""), EngString("item_key")) != 0;
}

struct SavedWeapon
{
	std::string cls;
	int clip;
	int slot; // posicao no inventario do CoF (inventorySlot), 99 se nao estiver la
};

static struct SavedInv
{
	bool valid;   // capturado no changelevel
	bool pending; // devolver quando o jogador nascer no mapa novo
	float restoreAt;
	float selectAt; // trocar para a arma ativa salva
	std::string netname;
	std::vector<SavedWeapon> weapons;
	std::vector<std::string> items; // itens que nao sao armas, na ordem dos slots
	std::string active;
	int ammo[AMMO_SLOTS];
	int flashBattery;
	int goodPoints;
} g_inv[33];

static void CaptureInventory(int i, edict_t* p, bool quiet = false)
{
	SavedInv& s = g_inv[i];
	void* pl = p->pvPrivateData;
	s.valid = true;
	s.pending = false;
	s.restoreAt = 0;
	s.netname = STRING(p->v.netname);
	s.weapons.clear();
	ForEachItem(pl, [&](void* it) {
		entvars_t* pev = PevOf(it);
		if (!pev || !pev->classname)
			return;
		SavedWeapon w = { STRING(pev->classname), Field<int>(it, OFS_ITEM_CLIP), 99 };
		// inventorySlot guarda "inventoryitems/weapons/<classe>.txt"
		std::string file = "/" + w.cls + ".txt";
		for (int k = 0; k < 7; k++)
		{
			int str = Field<int>(pl, OFS_INV_SLOT + k * 4);
			if (str && strstr(STRING(str), file.c_str()))
			{
				w.slot = k;
				break;
			}
		}
		s.weapons.push_back(w);
	});
	// devolver na mesma ordem dos slots: GiveNamedItem ocupa o primeiro slot livre
	std::stable_sort(s.weapons.begin(), s.weapons.end(),
		[](const SavedWeapon& a, const SavedWeapon& b) { return a.slot < b.slot; });
	s.items = ItemsOf(pl);
	entvars_t* act = PevOf(Field<void*>(pl, OFS_ACTIVE_ITEM));
	s.active = act ? STRING(act->classname) : "";
	memcpy(s.ammo, &Field<int>(pl, OFS_AMMO), sizeof(s.ammo));
	s.flashBattery = Field<int>(pl, OFS_FLASH_BATTERY);
	s.goodPoints = Field<int>(pl, OFS_GOOD_POINTS);
	if (!quiet)
		Log("inventario do jogador %d salvo: %d arma(s), %d item(ns), ativa '%s'", i, (int)s.weapons.size(),
		(int)s.items.size(), s.active.c_str());
}

static void RestoreInventory(int i, edict_t* p)
{
	SavedInv& s = g_inv[i];
	void* pl = p->pvPrivateData;
	int before[AMMO_SLOTS];
	memcpy(before, &Field<int>(pl, OFS_AMMO), sizeof(before));

	int given = 0, failed = 0;
	for (const SavedWeapon& w : s.weapons)
	{
		void* it = FindItem(pl, w.cls.c_str());
		if (it)
		{
			Field<int>(it, OFS_ITEM_CLIP) = max(Field<int>(it, OFS_ITEM_CLIP), w.clip);
			continue;
		}
		// GiveNamedItem guarda o ponteiro do nome como string_t: precisa ser string do engine.
		g_GiveNamedItem(pl, STRING(g_eng->pfnAllocString(w.cls.c_str())), false, true);
		it = FindItem(pl, w.cls.c_str());
		if (it)
		{
			Field<int>(it, OFS_ITEM_CLIP) = w.clip;
			given++;
		}
		else
		{
			failed++;
			Log("jogador %d: nao consegui devolver '%s'", i, w.cls.c_str());
		}
	}

	std::vector<std::string> have = ItemsOf(pl); // o slot sobrevive a morte: nao duplicar
	for (const std::string& f : s.items)
	{
		if (Has(have, f))
			continue;
		if (g_AddToInventory && GiveInvItem(pl, f))
			given++;
		else
		{
			failed++;
			Log("jogador %d: nao consegui devolver '%s'", i, f.c_str());
		}
	}

	int* ammo = &Field<int>(pl, OFS_AMMO);
	for (int k = 0; k < AMMO_SLOTS; k++)
		ammo[k] = max(before[k], s.ammo[k]);
	Field<int>(pl, OFS_FLASH_BATTERY) = s.flashBattery;
	Field<int>(pl, OFS_GOOD_POINTS) = s.goodPoints;

	Log("inventario do jogador %d devolvido: %d arma(s)/item(ns) dado(s), %d falha(s)", i, given, failed);
}

// Poe na mao a arma que o jogador segurava. O cliente do CoF nao aceita "weapon_x", entao
// usamos o "lastinv" do jogo apontando m_pLastItem para a arma desejada.
static void SelectWeapon(int i, edict_t* p, const std::string& cls)
{
	void* pl = p->pvPrivateData;
	entvars_t* act = PevOf(Field<void*>(pl, OFS_ACTIVE_ITEM));
	if (cls.empty() || !g_SelectLastItem || (act && cls == STRING(act->classname)))
		return;
	void* want = FindItem(pl, cls.c_str());
	if (!want)
		return;
	Field<void*>(pl, OFS_LAST_ITEM) = want;
	g_SelectLastItem(pl, true); // a troca termina depois da animacao de guardar a arma atual
	Log("jogador %d: trocando para '%s'", i, cls.c_str());
}

static void RestoreThink()
{
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		SavedInv& s = g_inv[i];
		edict_t* p = PlayerAt(i);
		if (s.selectAt && gpGlobals->time >= s.selectAt)
		{
			s.selectAt = 0;
			if (p && IsAlive(p))
				SelectWeapon(i, p, s.active);
		}
		if (!s.pending)
			continue;
		if (!p || !IsAlive(p))
		{
			s.restoreAt = 0;
			continue;
		}
		if (s.netname != STRING(p->v.netname))
		{
			s.pending = false; // outro jogador ocupou o slot
			continue;
		}
		// espera o spawn terminar de equipar o jogador antes de devolver
		if (!s.restoreAt)
			s.restoreAt = gpGlobals->time + 1.0f;
		if (gpGlobals->time < s.restoreAt)
			continue;
		s.pending = false;
		RestoreInventory(i, p);
		s.selectAt = gpGlobals->time + 0.5f; // deixa o deploy da ultima arma dada terminar
	}
}

// ---------------------------------------------------------------- chaves e itens compartilhados
//
// Os itens que nao sao armas (item_key: chaves, fitas, corda...) ficam num chaveiro do grupo. Quando
// alguem pega um item, todos recebem. Quando alguem gasta o item (porta destrancada, combinacao),
// ele some de todos. Assim ninguem fica preso atras de uma porta e o item nao se multiplica.
// Funciona por varredura do inventorySlot, sem hook: pega qualquer jeito de ganhar/perder item.

static std::vector<std::string> g_keyring;
static std::vector<std::string> g_seen[33]; // itens de cada jogador na ultima varredura
static bool g_seenValid[33];
static float g_nextShare;
static float g_giveRetryAt[33];

static std::string ItemName(const std::string& file)
{
	size_t a = file.find_last_of("/\\");
	std::string n = file.substr(a == std::string::npos ? 0 : a + 1);
	if (n.size() > 4 && !_stricmp(n.c_str() + n.size() - 4, ".txt"))
		n.resize(n.size() - 4);
	return n;
}

static void ShareThink()
{
	if (!cv_share_items.value || !g_AddToInventory || !g_RemoveInventoryItem || g_changing)
		return;
	if (gpGlobals->time < g_nextShare)
		return;
	g_nextShare = gpGlobals->time + 0.2f;

	// 1) o que cada jogador ganhou ou gastou desde a ultima varredura
	std::vector<std::string> used;
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* p = PlayerAt(i);
		if (!p || !IsAlive(p))
		{
			g_seenValid[i] = false; // morto/renascendo: inventario vazio nao conta como gasto
			continue;
		}
		std::vector<std::string> cur = ItemsOf(p->pvPrivateData);
		if (g_seenValid[i])
		{
			for (const std::string& f : g_seen[i])
				if (!Has(cur, f) && Has(g_keyring, f) && !Has(used, f))
				{
					used.push_back(f);
					Log("jogador %d gastou '%s' - removendo de todos", i, f.c_str());
				}
		}
		for (const std::string& f : cur)
			if (!Has(g_keyring, f))
			{
				g_keyring.push_back(f);
				char msg[160];
				_snprintf(msg, sizeof(msg), "%s pegou: %s", STRING(p->v.netname), ItemName(f).c_str());
				msg[sizeof(msg) - 1] = 0;
				Log("%s (%s)", msg, f.c_str());
				for (int j = 1; j <= gpGlobals->maxClients; j++)
				{
					edict_t* o = PlayerAt(j);
					if (o && j != i)
						g_eng->pfnClientPrintf(o, print_center, msg);
				}
			}
	}

	// 2) item gasto some do chaveiro e do inventario de todos
	for (const std::string& f : used)
	{
		g_keyring.erase(std::remove(g_keyring.begin(), g_keyring.end(), f), g_keyring.end());
		for (int j = 1; j <= gpGlobals->maxClients; j++)
		{
			edict_t* o = PlayerAt(j);
			if (o && IsAlive(o) && Has(ItemsOf(o->pvPrivateData), f))
				g_RemoveInventoryItem(o->pvPrivateData, EngString(f), false);
		}
	}

	// 3) todo mundo vivo recebe o que falta do chaveiro
	for (int j = 1; j <= gpGlobals->maxClients && j < 33; j++)
	{
		edict_t* o = PlayerAt(j);
		if (!o || !IsAlive(o))
			continue;
		void* pl = o->pvPrivateData;
		std::vector<std::string> cur = ItemsOf(pl);
		// inventario cheio: o AddToInventory falha (e manda mensagem ao cliente); tenta de novo so a cada 5 s
		if (gpGlobals->time >= g_giveRetryAt[j])
			for (const std::string& f : g_keyring)
				if (!Has(cur, f) && !GiveInvItem(pl, f))
				{
					g_giveRetryAt[j] = gpGlobals->time + 5.0f;
					Note("jogador %d: inventario sem espaco para '%s', tentando de novo em 5 s", j, f.c_str());
					break;
				}
		g_seen[j] = ItemsOf(pl);
		g_seenValid[j] = true;
	}
}

static void ResetKeyring()
{
	g_keyring.clear();
	for (int i = 0; i < 33; i++)
	{
		g_seen[i].clear();
		g_seenValid[i] = false;
		g_giveRetryAt[i] = 0;
	}
	g_nextShare = 0;
	g_strings.clear(); // as strings do engine morreram com o mapa anterior
}

// ---------------------------------------------------------------- revive / respawn / checkpoints
//
// O co-op nativo ja tem revive: injetar a seringa (weapon_syringe) num aliado caido chama
// CBasePlayer::StartRevive, que toca a animacao "revived" e levanta o jogador com 30 de vida.
// O jogador caido ve "Press jump to respawn (lose weapons) or wait to be revived".
// Aqui:
//  - ninguem renasce sozinho logo de cara: o grupo tem tempo de reviver;
//  - se o caido apertar pular (ou passar cofmp_respawn_time sem revive), renasce perto de um
//    companheiro vivo com os itens da ultima foto do inventario, em vez de perder as armas;
//  - quem volta a vida (revive ou respawn nativo) recebe de volta o que faltar da foto, e se o
//    respawn nativo o jogou longe, e levado para perto do grupo.
// A foto e tirada a cada segundo. Se todos morrem, o mapa reinicia e cada um recebe a foto.

static void CenterPrintAll(const char* msg);

typedef void(__thiscall* StartRevive_t)(void* player);
static StartRevive_t g_StartRevive;
static const DWORD RVA_StartRevive = 0xf1860;
static const BYTE SIG_StartRevive[] = { 0x56, 0x8B, 0xF1, 0x57, 0xC6, 0x86, 0x0C, 0x05, 0x00, 0x00, 0x00, 0xC7 };
static const float REVIVING = 3.0f; // pev->fuser3 durante a animacao de revive

static float g_deadSince[33];
static bool g_diedHere[33];   // morreu neste mapa (a proxima volta a vida e revive/respawn)
static bool g_aliveLast[33];
static Vector g_deathPos[33];
static float g_nextSnap;
static std::string g_lastMap; // para reconhecer o reinicio do mesmo mapa

static void SnapshotThink()
{
	if (gpGlobals->time < g_nextSnap || !g_GiveNamedItem)
		return;
	g_nextSnap = gpGlobals->time + 1.0f;
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* p = PlayerAt(i);
		// nao fotografa quem ainda espera receber o inventario de volta
		if (p && IsAlive(p) && !g_inv[i].pending && !g_inv[i].selectAt)
			CaptureInventory(i, p, true);
	}
}

static edict_t* PickBuddy(int self)
{
	for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
	{
		edict_t* o = PlayerAt(k);
		if (k != self && o && IsAlive(o))
			return o;
	}
	return nullptr;
}

static void PlaceNear(edict_t* p, edict_t* buddy)
{
	Vector pos = buddy->v.origin;
	if (FindFreeSpot(pos, p))
	{
		g_eng->pfnSetOrigin(p, pos);
		p->v.angles = buddy->v.angles;
		p->v.v_angle = buddy->v.v_angle;
		p->v.fixangle = 1;
	}
	p->v.velocity = Vector(0, 0, 0);
}

static void GiveBackItems(int i)
{
	if (g_inv[i].valid)
	{
		g_inv[i].pending = true;
		g_inv[i].restoreAt = 0;
	}
	g_seenValid[i] = false; // o ShareThink devolve as chaves do grupo
}

static void RespawnNear(int i, edict_t* p, edict_t* buddy)
{
	p->v.deadflag = DEAD_NO;
	p->v.iuser1 = 0;
	p->v.fuser3 = 0;
	g_orig.pfnSpawn(p); // CBasePlayer::Spawn
	PlaceNear(p, buddy);
	g_wasAlive[i] = true; // UnstickThink nao precisa mexer
	g_aliveLast[i] = true;
	g_diedHere[i] = false;
	g_deadSince[i] = 0;
	GiveBackItems(i);
	Log("jogador %d renasceu perto de %s", i, STRING(buddy->v.netname));
	char msg[128];
	_snprintf(msg, sizeof(msg), "%s voltou ao jogo", STRING(p->v.netname));
	CenterPrintAll(msg);
}

static void RespawnThink()
{
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* p = PlayerAt(i);
		if (!p)
		{
			g_aliveLast[i] = false;
			g_deadSince[i] = 0;
			continue;
		}
		bool alive = IsAlive(p);
		if (alive)
		{
			if (!g_aliveLast[i] && g_diedHere[i])
			{
				// voltou pelo jogo: revive (no lugar) ou respawn nativo (num spawn do mapa)
				g_diedHere[i] = false;
				bool away = (p->v.origin - g_deathPos[i]).Length() > 160.0f;
				edict_t* buddy = away ? PickBuddy(i) : nullptr;
				if (buddy)
					PlaceNear(p, buddy);
				GiveBackItems(i);
				Log("jogador %d %s", i, away ? "renasceu pelo jogo - levando para perto do grupo" : "foi revivido");
			}
			g_aliveLast[i] = true;
			g_deadSince[i] = 0;
			continue;
		}
		if (g_aliveLast[i])
		{
			g_aliveLast[i] = false;
			g_diedHere[i] = true;
			g_deathPos[i] = p->v.origin;
			g_deadSince[i] = gpGlobals->time;
			g_eng->pfnClientPrintf(p, print_center, "Espere ser revivido (seringa) ou pule para renascer perto do grupo com seus itens");
			continue;
		}
		if (!g_deadSince[i] || p->v.fuser3 == REVIVING)
			continue; // sendo revivido: nao interrompe a animacao
		float waited = gpGlobals->time - g_deadSince[i];
		bool jump = (p->v.button & IN_JUMP) && waited > 1.5f;
		bool timeout = cv_respawn_time.value > 0 && waited >= cv_respawn_time.value;
		if (!jump && !timeout)
			continue;
		edict_t* buddy = PickBuddy(i);
		if (buddy)
			RespawnNear(i, p, buddy); // todos mortos: o jogo reinicia o mapa
	}
}

static void ResetRespawn()
{
	memset(g_deadSince, 0, sizeof(g_deadSince));
	memset(g_diedHere, 0, sizeof(g_diedHere));
	memset(g_aliveLast, 0, sizeof(g_aliveLast));
	g_nextSnap = 0;
}

// ---------------------------------------------------------------- troca de mapa

// Quem estava longe de quem passou pela saida (ex.: o amigo abriu a porta e foi teleportado
// para a sala de saida) chega ao lado dele, nao no ponto equivalente ao lugar onde estava.
static const float NEAR_EXIT = 256.0f;

static void DoChangeLevel(int entIndex, edict_t* anchor = NULL)
{
	auto it = g_changelevels.find(entIndex);
	if (it == g_changelevels.end() || g_changing)
		return;
	const ChangeInfo& ci = it->second;
	StopSkip("troca de mapa");
	g_changing = true;
	g_gather.active = false;

	edict_t* lm = FindLandmark(ci.landmark);
	g_transit.pending = true;
	g_transit.map = ci.map;
	g_transit.landmark = lm ? ci.landmark : "";
	if (anchor && !(IsPlayer(anchor) && IsAlive(anchor)))
		anchor = NULL;
	g_transit.hasAnchor = lm && anchor;
	if (g_transit.hasAnchor)
	{
		g_transit.anchorOffset = anchor->v.origin - lm->v.origin;
		g_transit.anchorAngles = anchor->v.v_angle;
	}
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* p = PlayerAt(i);
		g_transit.has[i] = p && lm && IsAlive(p);
		if (g_transit.has[i])
		{
			bool away = anchor && (p->v.origin - anchor->v.origin).Length() > NEAR_EXIT;
			g_transit.offset[i] = away ? g_transit.anchorOffset : p->v.origin - lm->v.origin;
			g_transit.angles[i] = away ? g_transit.anchorAngles : p->v.v_angle;
		}
		g_inv[i].valid = false;
		if (p && IsAlive(p) && cv_keep_inv.value && g_GiveNamedItem)
			CaptureInventory(i, p);
	}

	Log("changelevel -> %s (landmark '%s')", ci.map.c_str(), ci.landmark.c_str());
	g_eng->pfnChangeLevel((char*)ci.map.c_str(), NULL);
}

// cofmp_map <mapa>: troca de mapa sem fechar o servidor nem o tunel (Home > Trocar mapa). Cada um leva o
// inventario e nasce no spawn do mapa novo.
static void Cmd_Map()
{
	const char* map = g_eng->pfnCmd_Argc() > 1 ? g_eng->pfnCmd_Argv(1) : "";
	if (gpGlobals->maxClients <= 1 || !*map)
	{
		Log("uso: cofmp_map <mapa> (com o servidor co-op aberto)");
		return;
	}
	if (!g_eng->pfnIsMapValid((char*)map))
	{
		Log("cofmp_map: o mapa '%s' nao existe", map);
		return;
	}
	if (g_changing)
		return;
	std::string name = map; // o Argv nao sobrevive ao resto
	StopSkip("troca de mapa");
	g_changing = true;
	g_gather.active = false;
	g_transit.pending = true;
	g_transit.map = name;
	g_transit.landmark = "";
	g_transit.hasAnchor = false;
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* p = PlayerAt(i);
		g_transit.has[i] = false;
		g_inv[i].valid = false;
		if (p && IsAlive(p) && cv_keep_inv.value && g_GiveNamedItem)
			CaptureInventory(i, p);
	}
	Log("cofmp_map -> %s", name.c_str());
	g_eng->pfnChangeLevel((char*)name.c_str(), NULL);
}

static void CenterPrintAll(const char* msg)
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		edict_t* p = PlayerAt(i);
		if (p)
			g_eng->pfnClientPrintf(p, print_center, msg);
	}
}

static void GatherThink()
{
	if (!g_gather.active || g_changing)
		return;

	edict_t* trig = g_eng->pfnPEntityOfEntIndex(g_gather.ent);
	if (IsNull(trig))
	{
		g_gather.active = false;
		return;
	}

	int alive = 0, inside = 0;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		edict_t* p = PlayerAt(i);
		if (!p || !IsAlive(p))
			continue;
		alive++;
		if (Overlaps(p, trig))
			inside++;
	}

	if (inside == 0 || alive == 0)
	{
		g_gather.active = false;
		CenterPrintAll(" ");
		return;
	}

	float timeout = cv_exit_timeout.value;
	if (inside >= alive || (timeout > 0 && gpGlobals->time - g_gather.start >= timeout))
	{
		DoChangeLevel(g_gather.ent, g_eng->pfnPEntityOfEntIndex(g_gather.who));
		return;
	}

	if (gpGlobals->time >= g_gather.nextMsg)
	{
		char msg[128];
		_snprintf(msg, sizeof(msg), "Aguardando jogadores na saida: %d/%d", inside, alive);
		CenterPrintAll(msg);
		g_gather.nextMsg = gpGlobals->time + 1.0f;
	}
}

// ---------------------------------------------------------------- detour ChangeLevelNow

typedef void(__thiscall* ChangeLevelNow_t)(void* self, void* pActivator);
static ChangeLevelNow_t g_ChangeLevelNowOrig; // trampolim

static void __fastcall Hook_ChangeLevelNow(void* self, void* /*edx*/, void* pActivator)
{
	if (!cv_enable.value || gpGlobals->maxClients <= 1)
	{
		g_ChangeLevelNowOrig(self, pActivator);
		return;
	}

	entvars_t* pev = *(entvars_t**)((char*)self + 4);
	if (!pev || g_changing)
		return;
	int idx = g_eng->pfnIndexOfEdict(pev->pContainingEntity);

	if (!g_changelevels.count(idx) || g_changelevels[idx].map.empty())
		return;

	edict_t* act = NULL;
	if (pActivator)
	{
		entvars_t* apev = *(entvars_t**)((char*)pActivator + 4);
		act = apev ? apev->pContainingEntity : NULL;
	}

	bool touch = act && IsPlayer(act) && Overlaps(act, pev->pContainingEntity);
	if (!touch || !cv_exit_wait.value)
	{
		DoChangeLevel(idx, act);
		return;
	}

	if (!g_gather.active || g_gather.ent != idx)
	{
		g_gather.active = true;
		g_gather.ent = idx;
		g_gather.who = g_eng->pfnIndexOfEdict(act);
		g_gather.start = gpGlobals->time;
		g_gather.nextMsg = 0;
		Log("jogador '%s' chegou na saida -> %s", STRING(act->v.netname), g_changelevels[idx].map.c_str());
	}
}

static const DWORD RVA_ChangeLevelNow = 0x109d40;
static const BYTE SIG_ChangeLevelNow[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x8B, 0x45, 0x08, 0x53, 0x8B, 0xD9 };
static const int STOLEN = 6; // push ebp / mov ebp,esp / sub esp,8

static void InstallDetour()
{
	static bool done;
	if (done)
		return;
	done = true;

	BYTE* give = (BYTE*)g_hl + RVA_GiveNamedItem;
	if (!memcmp(give, SIG_GiveNamedItem, sizeof(SIG_GiveNamedItem)))
		g_GiveNamedItem = (GiveNamedItem_t)give;
	else
		Log("ERRO: assinatura de GiveNamedItem nao confere - inventario entre mapas desativado");
	BYTE* sel = (BYTE*)g_hl + RVA_SelectLastItem;
	if (!memcmp(sel, SIG_SelectLastItem, sizeof(SIG_SelectLastItem)))
		g_SelectLastItem = (SelectLastItem_t)sel;
	else
		Log("aviso: assinatura de SelectLastItem nao confere - arma na mao nao sera restaurada");

	BYTE* add = (BYTE*)g_hl + RVA_AddToInventory;
	BYTE* rem = (BYTE*)g_hl + RVA_RemoveInventoryItem;
	if (!memcmp(add, SIG_AddToInventory, sizeof(SIG_AddToInventory)) &&
		SigMatch(rem, SIG_RemoveInventoryItem, sizeof(SIG_RemoveInventoryItem) - 1))
	{
		g_AddToInventory = (AddToInventory_t)add;
		g_RemoveInventoryItem = (RemoveInventoryItem_t)rem;
	}
	else
		Log("ERRO: assinatura de AddToInventory/RemoveInventoryItem nao confere - chaves compartilhadas desativadas");

	BYTE* rev = (BYTE*)g_hl + RVA_StartRevive;
	if (!memcmp(rev, SIG_StartRevive, sizeof(SIG_StartRevive)))
		g_StartRevive = (StartRevive_t)rev;
	else
		Log("aviso: assinatura de StartRevive nao confere - cofmp_revive desativado");

	BYTE* fn = (BYTE*)g_hl + RVA_ChangeLevelNow;
	if (memcmp(fn, SIG_ChangeLevelNow, sizeof(SIG_ChangeLevelNow)))
	{
		Log("ERRO: assinatura de ChangeLevelNow nao confere (versao diferente do hl.dll?) - detour desativado");
		return;
	}

	BYTE* tramp = (BYTE*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	memcpy(tramp, fn, STOLEN);
	tramp[STOLEN] = 0xE9;
	*(DWORD*)(tramp + STOLEN + 1) = (DWORD)(fn + STOLEN) - (DWORD)(tramp + STOLEN + 5);
	g_ChangeLevelNowOrig = (ChangeLevelNow_t)tramp;

	DWORD old;
	VirtualProtect(fn, STOLEN, PAGE_EXECUTE_READWRITE, &old);
	fn[0] = 0xE9;
	*(DWORD*)(fn + 1) = (DWORD)&Hook_ChangeLevelNow - (DWORD)(fn + 5);
	fn[5] = 0x90;
	VirtualProtect(fn, STOLEN, old, &old);
	FlushInstructionCache(GetCurrentProcess(), fn, STOLEN);
}

// ---------------------------------------------------------------- comandos de servidor

// cofmp_exits: lista os trigger_changelevel do mapa atual
static void Cmd_Exits()
{
	int n = 0;
	for (auto& kv : g_changelevels)
	{
		edict_t* e = g_eng->pfnPEntityOfEntIndex(kv.first);
		if (IsNull(e))
			continue;
		Vector c = (e->v.absmin + e->v.absmax) * 0.5f;
		Log("saida #%d -> %s (landmark '%s', flags %d) em %.0f %.0f %.0f", kv.first, kv.second.map.c_str(),
			kv.second.landmark.c_str(), e->v.spawnflags, c.x, c.y, c.z);
		n++;
	}
	if (!n)
		Log("nenhum trigger_changelevel neste mapa");
}

// cofmp_tp_exit <#>: leva todos os jogadores vivos para dentro da saida (destravar grupo / testes)
static void Cmd_TpExit()
{
	int idx = atoi(g_eng->pfnCmd_Argv(1));
	edict_t* e = g_eng->pfnPEntityOfEntIndex(idx);
	if (!g_changelevels.count(idx) || IsNull(e))
	{
		Log("uso: cofmp_tp_exit <#>  (veja cofmp_exits)");
		return;
	}
	Vector c = (e->v.absmin + e->v.absmax) * 0.5f;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		edict_t* p = PlayerAt(i);
		if (!p || !IsAlive(p))
			continue;
		Vector pos = c;
		if (FindFreeSpot(pos, p))
		{
			g_eng->pfnSetOrigin(p, pos);
			Log("jogador %d -> %.0f %.0f %.0f", i, pos.x, pos.y, pos.z);
		}
		else
			Log("jogador %d: sem espaco livre perto da saida", i);
	}
}

// cofmp_debug: estado dos slots de jogador
static void Cmd_Debug()
{
	Log("maxClients %d, time %.1f", gpGlobals->maxClients, gpGlobals->time);
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		edict_t* e = g_eng->pfnPEntityOfEntIndex(i);
		if (!e)
			continue;
		Log("slot %d: free %d flags 0x%x priv %p iuser1 %d health %.0f dead %d origin %.0f %.0f %.0f", i, e->free,
			e->v.flags, e->pvPrivateData, e->v.iuser1, e->v.health, e->v.deadflag, e->v.origin.x, e->v.origin.y,
			e->v.origin.z);
	}
}

// cofmp_inv: armas, municao e slots de inventario do CoF de cada jogador
static void Cmd_Inv()
{
	std::string ring;
	for (const std::string& f : g_keyring)
		ring += (ring.empty() ? "" : ", ") + ItemName(f);
	Log("chaveiro do grupo: %s", ring.empty() ? "(vazio)" : ring.c_str());
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		edict_t* p = PlayerAt(i);
		if (!p)
			continue;
		void* pl = p->pvPrivateData;
		entvars_t* act = PevOf(Field<void*>(pl, OFS_ACTIVE_ITEM));
		Log("jogador %d '%s': ativa '%s', lanterna %d, goodpoints %d", i, STRING(p->v.netname),
			act ? STRING(act->classname) : "", Field<int>(pl, OFS_FLASH_BATTERY), Field<int>(pl, OFS_GOOD_POINTS));
		ForEachItem(pl, [&](void* it) {
			entvars_t* pev = PevOf(it);
			Log("  arma %s id %d clip %d", pev ? STRING(pev->classname) : "?", Field<int>(it, OFS_ITEM_ID),
				Field<int>(it, OFS_ITEM_CLIP));
		});
		for (int k = 0; k < AMMO_SLOTS; k++)
		{
			int n = Field<int>(pl, OFS_AMMO + k * 4);
			if (n)
				Log("  municao[%d] = %d", k, n);
		}
		for (int k = 0; k < 7; k++)
		{
			int a = Field<int>(pl, OFS_INV_SLOT + k * 4), b = Field<int>(pl, OFS_INV_MODEL + k * 4),
				c = Field<int>(pl, OFS_INV_CLASS + k * 4);
			if (a || b || c)
				Log("  slot %d: '%s' modelo '%s' classe '%s'", k, a ? STRING(a) : "", b ? STRING(b) : "",
					c ? STRING(c) : "");
		}
	}
}

// cofmp_give <classname>: da um item a todos os jogadores vivos (testes)
static void Cmd_Give()
{
	const char* name = g_eng->pfnCmd_Argv(1);
	if (!g_GiveNamedItem || !name || !*name)
	{
		Log("uso: cofmp_give <classname>");
		return;
	}
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		edict_t* p = PlayerAt(i);
		if (p && IsAlive(p))
			g_GiveNamedItem(p->pvPrivateData, STRING(g_eng->pfnAllocString(name)), false, true);
	}
}

// cofmp_select <classname>: poe a arma na mao de todos os jogadores vivos (testes)
static void Cmd_Select()
{
	std::string name = g_eng->pfnCmd_Argv(1);
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		edict_t* p = PlayerAt(i);
		if (p && IsAlive(p))
			SelectWeapon(i, p, name);
	}
}

// cofmp_bot [nome]: cria um jogador falso (para testar a sincronizacao sozinho). Remova com "kick".
static void Cmd_Bot()
{
	const char* name = g_eng->pfnCmd_Argc() > 1 ? g_eng->pfnCmd_Argv(1) : "cofmp_bot";
	edict_t* b = g_eng->pfnCreateFakeClient(name);
	if (IsNull(b))
	{
		Log("nao ha slot livre para o bot (aumente maxplayers)");
		return;
	}
	char reject[128];
	g_orig.pfnClientConnect(b, name, "127.0.0.1", reject);
	g_orig.pfnClientPutInServer(b);
	b->v.flags |= FL_FAKECLIENT;
	Log("bot '%s' entrou no slot %d", name, g_eng->pfnIndexOfEdict(b));
}

static void BotThink()
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		edict_t* b = g_eng->pfnPEntityOfEntIndex(i);
		if (!IsNull(b) && (b->v.flags & FL_FAKECLIENT) && b->pvPrivateData)
			g_eng->pfnRunPlayerMove(b, b->v.v_angle, 0, 0, 0, 0, 0,
				(byte)max(1, min(255, (int)(gpGlobals->frametime * 1000))));
	}
}

// cofmp_use <classe> <x> <y> <z> [slot]: o jogador do slot (padrao: o primeiro humano vivo) "usa" a
// entidade dessa classe mais perto do ponto (pegar item_key, abrir inter_door) sem precisar mirar nela.
static void Cmd_Use()
{
	if (g_eng->pfnCmd_Argc() < 5)
	{
		Log("uso: cofmp_use <classe> <x> <y> <z> [slot]");
		return;
	}
	const char* cls = g_eng->pfnCmd_Argv(1);
	Vector at(atof(g_eng->pfnCmd_Argv(2)), atof(g_eng->pfnCmd_Argv(3)), atof(g_eng->pfnCmd_Argv(4)));
	edict_t* user = g_eng->pfnCmd_Argc() > 5 ? PlayerAt(atoi(g_eng->pfnCmd_Argv(5))) : NULL;
	for (int i = 1; i <= gpGlobals->maxClients && !user && g_eng->pfnCmd_Argc() <= 5; i++)
	{
		edict_t* p = PlayerAt(i);
		if (p && IsAlive(p) && !(p->v.flags & FL_FAKECLIENT))
			user = p;
	}
	edict_t* best = NULL;
	float bestDist = 1e9f;
	for (edict_t* e = g_eng->pfnFindEntityByString(NULL, "classname", cls); !IsNull(e);
		 e = g_eng->pfnFindEntityByString(e, "classname", cls))
	{
		Vector c = (e->v.absmin + e->v.absmax) * 0.5f;
		float d = (c - at).Length();
		if (d < bestDist)
			bestDist = d, best = e;
	}
	if (!user || !best)
	{
		Log("cofmp_use: %s", user ? "entidade nao encontrada" : "nenhum jogador vivo");
		return;
	}
	Log("jogador %d '%s' usa %s #%d (a %.0f unidades do ponto)", g_eng->pfnIndexOfEdict(user), STRING(user->v.netname), cls,
		g_eng->pfnIndexOfEdict(best), bestDist);
	g_orig.pfnUse(best, user);
}

// cofmp_after <segundos> <comando>: roda o comando no console do servidor depois do tempo (roteiros de teste)
static std::vector<std::pair<double, std::string>> g_after;

static void Cmd_After()
{
	if (g_eng->pfnCmd_Argc() < 3)
	{
		Log("uso: cofmp_after <segundos> \"<comando>\"");
		return;
	}
	std::string cmd;
	for (int k = 2; k < g_eng->pfnCmd_Argc(); k++)
		cmd += std::string(k > 2 ? " " : "") + g_eng->pfnCmd_Argv(k);
	g_after.push_back({ Plat_Time() + atof(g_eng->pfnCmd_Argv(1)), cmd + "\n" });
}

static void AfterThink()
{
	for (size_t k = 0; k < g_after.size();)
	{
		if (Plat_Time() < g_after[k].first)
		{
			k++;
			continue;
		}
		std::string cmd = g_after[k].second;
		g_after.erase(g_after.begin() + k);
		g_eng->pfnServerCommand((char*)cmd.c_str());
	}
}

// cofmp_tp <x> <y> <z>: leva o primeiro jogador humano vivo para o ponto livre mais perto (testes)
// cofmp_kill <n>: mata o jogador n (testes de revive com bot)
static void Cmd_Kill()
{
	edict_t* p = PlayerAt(atoi(g_eng->pfnCmd_Argv(1)));
	if (p && IsAlive(p))
		g_orig.pfnClientKill(p);
}

// cofmp_revive <n>: chama o revive nativo (o mesmo da seringa) no jogador n caido
static void Cmd_Revive()
{
	edict_t* p = PlayerAt(atoi(g_eng->pfnCmd_Argv(1)));
	if (!p || IsAlive(p) || !g_StartRevive)
	{
		Log("cofmp_revive: jogador nao esta caido");
		return;
	}
	g_StartRevive(p->pvPrivateData);
	Log("cofmp_revive: revivendo jogador %s", STRING(p->v.netname));
}

static void Cmd_Tp()
{
	if (g_eng->pfnCmd_Argc() < 4)
	{
		Log("uso: cofmp_tp <x> <y> <z>");
		return;
	}
	Vector pos(atof(g_eng->pfnCmd_Argv(1)), atof(g_eng->pfnCmd_Argv(2)), atof(g_eng->pfnCmd_Argv(3)));
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		edict_t* p = PlayerAt(i);
		if (!p || !IsAlive(p) || (p->v.flags & FL_FAKECLIENT))
			continue;
		if (FindFreeSpot(pos, p))
		{
			g_eng->pfnSetOrigin(p, pos);
			Log("jogador %d -> %.0f %.0f %.0f", i, pos.x, pos.y, pos.z);
		}
		else
			Log("jogador %d: sem espaco livre perto do ponto", i);
		return;
	}
}

// ---------------------------------------------------------------- comeco junto
//
// Muitos mapas comecam com uma cutscene disparada por trigger_auto ou por um trigger_once em volta do
// info_player_start (o c_intro, por exemplo). Em MP o host nasce primeiro e a cena roda antes de o amigo
// entrar. Entao, no comeco de cada mapa, os gatilhos ficam parados e os jogadores congelados ate o grupo
// todo nascer:
//  - servidor aberto agora: espera maxplayers jogadores, ou o host mandar cofmp_go (Home > Comecar agora);
//  - troca de mapa ou reinicio: espera quem estava no mapa anterior (no maximo cofmp_start_timeout s).

static struct
{
	bool active;
	int want;
	float deadline; // 0 = sem limite
	float releaseAt;
	float nextMsg;
	bool frozen[33]; // congelados por nos
} g_hold;
static int g_humansLast = -1; // humanos no mapa; no ServerActivate ainda e o valor do mapa anterior

static int CountHumans(bool aliveOnly)
{
	int n = 0;
	for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
	{
		edict_t* o = PlayerAt(k);
		if (o && !(o->v.flags & FL_FAKECLIENT) && (!aliveOnly || IsAlive(o)))
			n++;
	}
	return n;
}

static void StartHold(bool fresh)
{
	memset(&g_hold, 0, sizeof(g_hold));
	if (!cv_enable.value || !cv_start_wait.value || gpGlobals->maxClients <= 1)
		return;
	g_hold.active = true;
	if (fresh)
		g_hold.want = gpGlobals->maxClients;
	else
	{
		g_hold.want = g_humansLast > 1 ? g_humansLast : 1;
		g_hold.deadline = gpGlobals->time + cv_start_timeout.value;
	}
	Log("segurando o comeco do mapa ate %d jogador(es) nascerem%s", g_hold.want,
		fresh ? " (ou cofmp_go)" : "");
}

static void ReleaseHold(const char* why)
{
	if (!g_hold.active)
		return;
	g_hold.active = false;
	for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
	{
		edict_t* o = PlayerAt(k);
		if (o && g_hold.frozen[k])
			o->v.flags &= ~FL_FROZEN;
	}
	CenterPrintAll(" ");
	Log("comeco do mapa liberado (%s)", why);
}

static void HoldThink()
{
	if (!g_hold.active)
	{
		g_humansLast = CountHumans(false);
		return;
	}
	for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
	{
		edict_t* o = PlayerAt(k);
		if (o && IsAlive(o) && !(o->v.flags & FL_FROZEN))
		{
			o->v.flags |= FL_FROZEN;
			g_hold.frozen[k] = true;
		}
	}
	int have = CountHumans(true);
	if (!g_hold.releaseAt && (have >= g_hold.want || (g_hold.deadline && gpGlobals->time > g_hold.deadline)))
		g_hold.releaseAt = gpGlobals->time + 2.0f; // o ultimo a nascer ainda esta abrindo a tela
	if (g_hold.releaseAt && gpGlobals->time >= g_hold.releaseAt)
		ReleaseHold(have >= g_hold.want ? "todos nasceram" : "tempo esgotado");
	else if (gpGlobals->time >= g_hold.nextMsg)
	{
		g_hold.nextMsg = gpGlobals->time + 1.0f;
		char msg[160];
		_snprintf(msg, sizeof(msg), "Esperando o grupo: %d/%d\nO host pode comecar antes: Home > Comecar agora",
			have, g_hold.want);
		msg[sizeof(msg) - 1] = 0;
		CenterPrintAll(msg);
	}
}

static bool CutWaiting();
static void FireCutWait(const char* why);

// cofmp_go: comeca o mapa (ou a cutscene) sem esperar mais ninguem
static void Cmd_Go()
{
	if (g_hold.active)
		ReleaseHold("cofmp_go");
	if (CutWaiting())
		FireCutWait("cofmp_go");
}

static bool IsTriggerEnt(edict_t* e)
{
	return !IsNull(e) && !strncmp(STRING(e->v.classname), "trigger_", 8);
}

// ---------------------------------------------------------------- imortal
//
// Motivos para um jogador nao tomar dano (esperando o grupo para uma cutscene, camera ligada). Enquanto houver
// algum, takedamage fica DAMAGE_NO, FL_GODMODE ligado e a vida nao cai; quando o ultimo sai, volta como estava.

enum { GOD_WAIT = 1, GOD_CAM = 2 };
static struct
{
	int why;
	float takedamage;
	float health;
} g_god[33];

static void SetGod(int i, int why, bool on)
{
	if (i < 1 || i >= 33)
		return;
	edict_t* p = PlayerAt(i);
	if (!p)
	{
		g_god[i] = {};
		return;
	}
	int before = g_god[i].why;
	g_god[i].why = on ? before | why : before & ~why;
	if (!before && g_god[i].why)
	{
		g_god[i].takedamage = p->v.takedamage;
		g_god[i].health = p->v.health;
		p->v.takedamage = DAMAGE_NO;
		p->v.flags |= FL_GODMODE;
		Note("jogador %d imortal (%s)", i, why == GOD_WAIT ? "esperando o grupo" : "cutscene");
	}
	else if (before && !g_god[i].why)
	{
		p->v.takedamage = g_god[i].takedamage;
		p->v.flags &= ~FL_GODMODE;
		Note("jogador %d mortal de novo", i);
	}
}

static int g_camOwner; // jogador que disparou a camera (0 = nenhuma)

// o jogo pode mexer no takedamage (renascer, scripts): reforca a cada frame
static void GodThink()
{
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		if ((g_god[i].why & GOD_CAM) && !g_camOwner) // camera desligou sem passar pelo SetView
			SetGod(i, GOD_CAM, false);
		if (!g_god[i].why)
			continue;
		edict_t* p = PlayerAt(i);
		if (!p || !IsAlive(p))
			continue;
		p->v.takedamage = DAMAGE_NO;
		p->v.flags |= FL_GODMODE;
		if (p->v.health < g_god[i].health)
			p->v.health = g_god[i].health;
		else
			g_god[i].health = p->v.health;
	}
}

static void ResetGod()
{
	memset(g_god, 0, sizeof(g_god));
}

// ---------------------------------------------------------------- espera antes das cutscenes
//
// No SP a cutscene comeca quando o jogador entra num trigger_once/trigger_multiple cuja cadeia de alvos
// (multi_manager, trigger_relay) chega num trigger_camera. Em co-op, quem chega primeiro fica parado e imortal
// dentro do gatilho ate todos os vivos chegarem perto dele; ai o gatilho dispara normalmente.
// O host pode liberar antes com cofmp_go (Home > Comecar agora).

struct EntKV
{
	std::string cls, name, master;
	int spawnflags = 0;
	std::vector<std::string> outs; // nomes que ele dispara
};
static std::unordered_map<int, EntKV> g_kv; // por indice do edict, lido no KeyValue
static std::unordered_map<int, std::string> g_cutTrig; // gatilho de cutscene -> camera

static void RecordKeyValue(edict_t* ent, KeyValueData* kvd)
{
	EntKV& e = g_kv[g_eng->pfnIndexOfEdict(ent)];
	e.cls = kvd->szClassName;
	const char* k = kvd->szKeyName;
	if (!_stricmp(k, "targetname"))
		e.name = kvd->szValue;
	else if (!_stricmp(k, "target"))
		e.outs.push_back(kvd->szValue);
	else if (!_stricmp(k, "master"))
		e.master = kvd->szValue;
	else if (!_stricmp(k, "spawnflags"))
		e.spawnflags = atoi(kvd->szValue);
	else if (!_stricmp(e.cls.c_str(), "multi_manager") && _stricmp(k, "classname") && _stricmp(k, "origin") &&
		_stricmp(k, "wait") && _stricmp(k, "delay") && _stricmp(k, "killtarget") && _strnicmp(k, "zhlt", 4))
	{
		std::string t = k; // "alvo#2" = segundo disparo do mesmo alvo
		size_t h = t.find('#');
		if (h != std::string::npos)
			t.resize(h);
		e.outs.push_back(t);
	}
}

static void FindCutsceneTriggers()
{
	g_cutTrig.clear();
	std::unordered_map<std::string, std::vector<int>> byName;
	for (auto& kv : g_kv)
		if (!kv.second.name.empty())
			byName[kv.second.name].push_back(kv.first);
	for (auto& kv : g_kv)
	{
		const EntKV& t = kv.second;
		// spawnflags 2 = "no clients"; com master (multisource) o gatilho pode estar travado: nao segura ninguem
		if ((_stricmp(t.cls.c_str(), "trigger_once") && _stricmp(t.cls.c_str(), "trigger_multiple")) ||
			(t.spawnflags & 2) || !t.master.empty())
			continue;
		std::vector<std::pair<std::string, int>> q;
		for (const std::string& o : t.outs)
			q.push_back({ o, 0 });
		std::vector<std::string> seen;
		std::string cam;
		for (size_t n = 0; n < q.size() && cam.empty(); n++)
		{
			if (q[n].second > 6 || Has(seen, q[n].first))
				continue;
			seen.push_back(q[n].first);
			for (int idx : byName[q[n].first])
			{
				const EntKV& x = g_kv[idx];
				if (!_stricmp(x.cls.c_str(), "trigger_camera"))
				{
					cam = q[n].first;
					break;
				}
				if (!_stricmp(x.cls.c_str(), "multi_manager") || !_stricmp(x.cls.c_str(), "trigger_relay"))
					for (const std::string& o : x.outs)
						q.push_back({ o, q[n].second + 1 });
			}
		}
		if (!cam.empty())
			g_cutTrig[kv.first] = cam;
	}
	g_kv.clear();
}

static struct
{
	edict_t* trig; // gatilho segurado agora
	bool waiting[33]; // jogadores parados nele
	bool frozen[33]; // congelados por nos
	float nextMsg;
} g_cut;

static bool CutWaiting()
{
	return g_cut.trig != NULL;
}

static bool NearTrigger(edict_t* p, edict_t* trig)
{
	const float m = 96.0f;
	for (int k = 0; k < 3; k++)
		if (p->v.absmin[k] > trig->v.absmax[k] + m || p->v.absmax[k] < trig->v.absmin[k] - m)
			return false;
	return true;
}

static void EndCutWait(const char* why)
{
	for (int k = 1; k < 33; k++)
	{
		edict_t* o = k <= gpGlobals->maxClients ? PlayerAt(k) : NULL;
		if (o && g_cut.frozen[k])
			o->v.flags &= ~FL_FROZEN;
		if (g_cut.waiting[k])
			SetGod(k, GOD_WAIT, false);
	}
	if (g_cut.trig && why)
	{
		Log("cutscene: gatilho #%d liberado (%s)", g_eng->pfnIndexOfEdict(g_cut.trig), why);
		CenterPrintAll(" ");
	}
	memset(&g_cut, 0, sizeof(g_cut));
}

// true = segura o toque
static bool CutWaitTouch(edict_t* trig, edict_t* pl)
{
	if (!cv_enable.value || !cv_cutscene_wait.value || gpGlobals->maxClients <= 1 || !IsPlayer(pl) || !IsAlive(pl) ||
		(pl->v.flags & FL_FAKECLIENT))
		return false;
	auto it = g_cutTrig.find(g_eng->pfnIndexOfEdict(trig));
	if (it == g_cutTrig.end())
		return false;
	if (g_cut.trig && g_cut.trig != trig)
		return true; // outro gatilho de cutscene ja esta esperando: um de cada vez
	int i = g_eng->pfnIndexOfEdict(pl);
	if (!g_cut.trig)
	{
		g_cut.trig = trig;
		Log("cutscene: '%s' chegou no gatilho #%d (camera '%s'); esperando o grupo", STRING(pl->v.netname),
			g_eng->pfnIndexOfEdict(trig), it->second.c_str());
	}
	if (!g_cut.waiting[i])
	{
		g_cut.waiting[i] = true;
		SetGod(i, GOD_WAIT, true);
	}
	if (!(pl->v.flags & FL_FROZEN))
	{
		pl->v.flags |= FL_FROZEN;
		pl->v.velocity = Vector(0, 0, 0);
		g_cut.frozen[i] = true;
	}
	return true;
}

// dispara o gatilho segurado como se o primeiro que chegou tivesse acabado de tocar nele
static void FireCutWait(const char* why)
{
	edict_t* trig = g_cut.trig;
	edict_t* act = NULL;
	for (int k = 1; k <= gpGlobals->maxClients && k < 33 && !act; k++)
	{
		edict_t* o = PlayerAt(k);
		if (o && g_cut.waiting[k] && IsAlive(o))
			act = o;
	}
	g_cutTrig.erase(g_eng->pfnIndexOfEdict(trig)); // trigger_multiple nao segura de novo
	EndCutWait(why);
	if (act)
		g_orig.pfnTouch(trig, act);
}

static void CutWaitThink()
{
	if (!g_cut.trig)
		return;
	if (IsNull(g_cut.trig) || g_cut.trig->free)
	{
		EndCutWait("gatilho sumiu");
		return;
	}
	// todo humano no servidor conta, inclusive quem esta morto esperando renascer (cofmp_cutscene_wait 2: bots
	// tambem, para testar sozinho)
	int all = 0, here = 0, waitingAlive = 0;
	std::string missing;
	for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
	{
		edict_t* o = PlayerAt(k);
		if (!o || ((o->v.flags & FL_FAKECLIENT) && cv_cutscene_wait.value < 2))
			continue;
		all++;
		bool alive = IsAlive(o);
		if (g_cut.waiting[k] && alive)
			waitingAlive++;
		if (alive && (g_cut.waiting[k] || NearTrigger(o, g_cut.trig)))
			here++;
		else if (missing.size() < 80)
			missing += std::string(missing.empty() ? "" : ", ") + STRING(o->v.netname) + (alive ? "" : " (morto)");
	}
	if (!waitingAlive)
	{
		EndCutWait("ninguem mais esperando");
		return;
	}
	if (here >= all)
	{
		char why[64];
		_snprintf(why, sizeof(why), "todos chegaram: %d/%d", here, all);
		why[sizeof(why) - 1] = 0;
		FireCutWait(why);
		return;
	}
	if (gpGlobals->time < g_cut.nextMsg)
		return;
	g_cut.nextMsg = gpGlobals->time + 1.0f;
	for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
	{
		edict_t* o = PlayerAt(k);
		if (!o || (o->v.flags & FL_FAKECLIENT))
			continue;
		char msg[200];
		if (g_cut.waiting[k])
			_snprintf(msg, sizeof(msg), "Esperando o grupo para a cutscene (%d/%d)\nFaltam: %s\nO host pode comecar antes: Home > Comecar agora",
				here, all, missing.c_str());
		else
			_snprintf(msg, sizeof(msg), "Seu grupo esta esperando voce para uma cutscene (%d/%d)", here, all);
		msg[sizeof(msg) - 1] = 0;
		g_eng->pfnClientPrintf(o, print_center, msg);
	}
}

static bool PickupTouch(edict_t* item, edict_t* player);

static void Hook_Touch(edict_t* touched, edict_t* other)
{
	if (g_hold.active && ((IsTriggerEnt(touched) && IsPlayer(other)) || (IsTriggerEnt(other) && IsPlayer(touched))))
		return;
	if (!IsNull(touched) && !IsNull(other) && CutWaitTouch(touched, other))
		return;
	if (PickupTouch(touched, other))
		return;
	g_orig.pfnTouch(touched, other);
	if (g_tracing)
	{
		char during[160];
		_snprintf(during, sizeof(during), "touch de '%s' #%d com '%s' #%d", STRING(touched->v.classname),
			g_eng->pfnIndexOfEdict(touched), STRING(other->v.classname), g_eng->pfnIndexOfEdict(other));
		during[sizeof(during) - 1] = 0;
		TraceCheck(during);
	}
}

static void Hook_Think(edict_t* ent)
{
	// o engine zera nextthink antes de chamar Think: reagenda para tentar de novo
	if (g_hold.active && !IsNull(ent) && !strcmp(STRING(ent->v.classname), "trigger_auto"))
	{
		ent->v.nextthink = gpGlobals->time + 0.1f;
		return;
	}
	g_orig.pfnThink(ent);
	if (g_tracing)
	{
		char during[128];
		_snprintf(during, sizeof(during), "think de '%s' #%d", STRING(ent->v.classname), g_eng->pfnIndexOfEdict(ent));
		during[sizeof(during) - 1] = 0;
		TraceCheck(during);
	}
}

// ---------------------------------------------------------------- hooks DLL_FUNCTIONS

static void Hook_GameInit()
{
	g_eng->pfnCVarRegister(&cv_enable);
	g_eng->pfnCVarRegister(&cv_exit_wait);
	g_eng->pfnCVarRegister(&cv_exit_timeout);
	g_eng->pfnCVarRegister(&cv_keep_inv);
	g_eng->pfnCVarRegister(&cv_share_items);
	g_eng->pfnCVarRegister(&cv_respawn_time);
	g_eng->pfnCVarRegister(&cv_model);
	g_eng->pfnCVarRegister(&cv_start_wait);
	g_eng->pfnCVarRegister(&cv_start_timeout);
	g_eng->pfnCVarRegister(&cv_cutscene_wait);
	g_eng->pfnAddServerCommand((char*)"cofmp_exits", Cmd_Exits);
	g_eng->pfnAddServerCommand((char*)"cofmp_tp_exit", Cmd_TpExit);
	g_eng->pfnAddServerCommand((char*)"cofmp_debug", Cmd_Debug);
	g_eng->pfnAddServerCommand((char*)"cofmp_inv", Cmd_Inv);
	g_eng->pfnAddServerCommand((char*)"cofmp_give", Cmd_Give);
	g_eng->pfnAddServerCommand((char*)"cofmp_select", Cmd_Select);
	g_eng->pfnAddServerCommand((char*)"cofmp_bot", Cmd_Bot);
	g_eng->pfnAddServerCommand((char*)"cofmp_use", Cmd_Use);
	g_eng->pfnAddServerCommand((char*)"cofmp_tp", Cmd_Tp);
	g_eng->pfnAddServerCommand((char*)"cofmp_kill", Cmd_Kill);
	g_eng->pfnAddServerCommand((char*)"cofmp_revive", Cmd_Revive);
	g_eng->pfnAddServerCommand((char*)"cofmp_after", Cmd_After);
	g_eng->pfnAddServerCommand((char*)"cofmp_go", Cmd_Go);
	g_eng->pfnAddServerCommand((char*)"cofmp_map", Cmd_Map);
	g_orig.pfnGameInit();
	Log("carregado (co-op da campanha)");
}

static void Hook_KeyValue(edict_t* ent, KeyValueData* kvd)
{
	if (kvd->szClassName && kvd->szKeyName && kvd->szValue)
	{
		if (!_stricmp(kvd->szClassName, "worldspawn"))
		{
			g_changelevels.clear();
			g_kv.clear();
		}
		else
		{
			RecordKeyValue(ent, kvd);
			if (!_stricmp(kvd->szClassName, "trigger_changelevel"))
			{
				ChangeInfo& ci = g_changelevels[g_eng->pfnIndexOfEdict(ent)];
				if (!_stricmp(kvd->szKeyName, "map"))
					ci.map = kvd->szValue;
				else if (!_stricmp(kvd->szKeyName, "landmark"))
					ci.landmark = kvd->szValue;
			}
		}
	}
	g_orig.pfnKeyValue(ent, kvd);
}

static void ResetCutscene();
static void Hook_SetView(const edict_t* client, const edict_t* view);
static void ResetTrace();
static bool g_netReset;

static void Hook_ServerActivate(edict_t* list, int count, int clientMax)
{
	g_orig.pfnServerActivate(list, count, clientMax);

	StopSkip("mapa novo");
	ResetTrace();
	g_netReset = true; // o carregamento do mapa nao conta como travada
	g_changing = false;
	g_gather.active = false;
	memset(&g_arrive, 0, sizeof(g_arrive));
	memset(g_wasAlive, 0, sizeof(g_wasAlive));
	memset(g_unstickAt, 0, sizeof(g_unstickAt));
	ResetKeyring();
	ResetCutscene();
	ResetGod();
	memset(&g_cut, 0, sizeof(g_cut));
	FindCutsceneTriggers();

	const char* map = STRING(gpGlobals->mapname);
	bool arrived = g_transit.pending && !_stricmp(map, g_transit.map.c_str());
	// mesmo mapa de novo sem changelevel nosso = todos morreram: volta ao checkpoint
	bool restart = !arrived && !_stricmp(map, g_lastMap.c_str());
	g_lastMap = map;
	ResetRespawn();
	for (int i = 1; i < 33; i++)
	{
		g_inv[i].pending = (arrived || restart) && g_inv[i].valid;
		g_inv[i].valid = false;
		g_inv[i].restoreAt = 0;
		g_inv[i].selectAt = 0;
	}
	if (arrived)
	{
		edict_t* lm = FindLandmark(g_transit.landmark);
		if (lm)
		{
			g_arrive.active = true;
			g_arrive.landmark = lm->v.origin;
			g_arrive.deadline = gpGlobals->time + 120.0f;
			for (int i = 1; i < 33; i++)
				g_arrive.place[i] = g_transit.has[i];
		}
	}
	g_transit.pending = false;

	// servidor recem-aberto (ninguem veio do mapa anterior) espera a lotacao; troca de mapa espera o grupo
	if (_strnicmp(map, "c_game_menu", 11))
		StartHold(g_humansLast <= 0 && !arrived && !restart);
	else
		memset(&g_hold, 0, sizeof(g_hold));

	// -cofmp_test <cfg>: roda um roteiro de teste no primeiro mapa de campanha (com cofmp_after)
	static bool testDone;
	const char* test = strstr(GetCommandLineA(), "-cofmp_test ");
	if (test && !testDone && _strnicmp(map, "c_game_menu", 11))
	{
		testDone = true;
		char cmd[128];
		_snprintf(cmd, sizeof(cmd), "exec %.64s\n", test + 12);
		cmd[sizeof(cmd) - 1] = 0;
		if (char* sp = strchr(cmd + 5, ' '))
			strcpy(sp, "\n");
		g_eng->pfnServerCommand(cmd);
	}

	Log("mapa %s: %d trigger_changelevel, %d gatilho(s) de cutscene%s%s", map, (int)g_changelevels.size(),
		(int)g_cutTrig.size(), g_arrive.active ? ", posicionando jogadores pelo landmark" : "",
		restart ? ", reinicio: devolvendo inventarios do checkpoint" : "");
}

static void ArriveThink()
{
	if (!g_arrive.active)
		return;
	if (gpGlobals->time > g_arrive.deadline)
	{
		g_arrive.active = false;
		return;
	}

	bool any = false;
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		if (!g_arrive.place[i])
			continue;
		any = true;
		edict_t* p = PlayerAt(i);
		if (!p || !IsAlive(p))
		{
			g_arrive.seen[i] = 0;
			continue;
		}
		if (!g_arrive.seen[i])
		{
			g_arrive.seen[i] = gpGlobals->time;
			continue;
		}
		if (gpGlobals->time - g_arrive.seen[i] < 0.5f)
			continue;

		Vector pos = g_arrive.landmark + g_transit.offset[i];
		bool ok = FindFreeSpot(pos, p);
		if (!ok && g_transit.hasAnchor)
		{
			pos = g_arrive.landmark + g_transit.anchorOffset;
			g_transit.angles[i] = g_transit.anchorAngles;
			ok = FindFreeSpot(pos, p);
		}
		if (ok)
		{
			g_eng->pfnSetOrigin(p, pos);
			p->v.velocity = Vector(0, 0, 0);
			p->v.angles = g_transit.angles[i];
			p->v.v_angle = g_transit.angles[i];
			p->v.fixangle = 1;
			Log("jogador %d chegou pelo landmark em %.0f %.0f %.0f", i, pos.x, pos.y, pos.z);
		}
		else
			Log("jogador %d: sem espaco livre perto do landmark, mantendo spawn", i);
		g_arrive.place[i] = false;
		g_unstickAt[i] = gpGlobals->time + 0.2f;
	}
	if (!any)
		g_arrive.active = false;
}

// Jogadores que (re)nascem em cima de outro ficam presos: empurra para um ponto livre.
static void UnstickThink()
{
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* p = PlayerAt(i);
		bool alive = p && IsAlive(p);
		if (alive && !g_wasAlive[i] && !g_unstickAt[i])
			g_unstickAt[i] = gpGlobals->time + 0.3f;
		g_wasAlive[i] = alive;

		if (!alive || !g_unstickAt[i] || gpGlobals->time < g_unstickAt[i] || g_arrive.place[i])
			continue;
		g_unstickAt[i] = 0;
		if (HullFree(p->v.origin, p))
			continue;
		Vector pos = p->v.origin;
		if (FindFreeSpot(pos, p))
		{
			g_eng->pfnSetOrigin(p, pos);
			Log("desprendendo jogador %d", i);
		}
	}
}

static void CamJoinThink();
static void SkipThink();
static void RequestSkip(int i);
static void TraceThink();
static void InvWatchThink();
static void TraceDrop(edict_t* e);
static void DropCommand(edict_t* e);

// ---------------------------------------------------------------- log de rede e desempenho
//
// A cada 10 s vai para o cofmp_log.txt o ping e a perda de cada jogador e os frames do servidor; um frame
// que demora mais de 250 ms (travada) e anotado na hora.

static void NetStatsThink()
{
	static LARGE_INTEGER freq, last, lastReport;
	static int frames;
	static double worst;
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (!freq.QuadPart || g_netReset)
	{
		QueryPerformanceFrequency(&freq);
		last = lastReport = now;
		frames = 0;
		worst = 0;
		g_netReset = false;
		return;
	}
	double dt = (double)(now.QuadPart - last.QuadPart) / freq.QuadPart;
	last = now;
	frames++;
	if (dt > worst)
		worst = dt;
	if (dt > 0.25 && !g_changing && gpGlobals->maxClients > 1)
		Note("servidor travou %.0f ms num frame (mapa %s)", dt * 1000, STRING(gpGlobals->mapname));
	double span = (double)(now.QuadPart - lastReport.QuadPart) / freq.QuadPart;
	if (span < 10.0 || gpGlobals->maxClients <= 1)
		return;
	char line[400];
	int n = _snprintf(line, sizeof(line), "10s: servidor %.0f fps (pior frame %.0f ms)", frames / span, worst * 1000);
	for (int k = 1; k <= gpGlobals->maxClients && k < 33 && n > 0 && n < (int)sizeof(line) - 80; k++)
	{
		edict_t* o = PlayerAt(k);
		if (!o || (o->v.flags & FL_FAKECLIENT))
			continue;
		int ping = 0, loss = 0;
		g_eng->pfnGetPlayerStats(o, &ping, &loss);
		n += _snprintf(line + n, sizeof(line) - n, "; %d '%.20s' ping %d perda %d%%%s", k, STRING(o->v.netname), ping,
			loss, IsAlive(o) ? "" : " (morto)");
	}
	line[sizeof(line) - 1] = 0;
	Note("%s", line);
	frames = 0;
	worst = 0;
	lastReport = now;
}

static void Hook_StartFrame()
{
	Hud_RunPending();
	g_orig.pfnStartFrame();
	AfterThink();
	NetStatsThink();
	if (!cv_enable.value || gpGlobals->maxClients <= 1)
		return;
	SkipThink();
	TraceThink();
	InvWatchThink();
	BotThink();
	HoldThink();
	CutWaitThink();
	GodThink();
	CamJoinThink();
	GatherThink();
	ArriveThink();
	UnstickThink();
	RestoreThink();
	ShareThink();
	RespawnThink();
	SnapshotThink();
}

// ---------------------------------------------------------------- modelo do jogador
//
// O co-op do CoF veste todos de policial (o cliente manda "model police1..4"). O modelo dos outros
// jogadores e escolhido pelo cliente a partir do "model" do userinfo, entao trocamos esse valor
// para models/player/simon/simon.mdl (gerado pelo instalador com a malha do Simon e as animacoes
// do policial). Quem nao tem o arquivo ve o modelo padrao, como antes.

static void ForceModel(edict_t* e, char* info)
{
	const char* want = cv_model.string;
	// bot nao tem canal de rede: a atualizacao do userinfo dele derruba o servidor na troca de mapa
	if (!cv_enable.value || !want || !*want || !info || gpGlobals->maxClients <= 1 || (e->v.flags & FL_FAKECLIENT))
		return;
	if (_stricmp(g_eng->pfnInfoKeyValue(info, (char*)"model"), want))
		g_eng->pfnSetClientKeyValue(g_eng->pfnIndexOfEdict(e), info, (char*)"model", (char*)want);
}

static void Hook_ClientUserInfoChanged(edict_t* e, char* info)
{
	g_orig.pfnClientUserInfoChanged(e, info);
	ForceModel(e, info);
}

static void Hook_ClientPutInServer(edict_t* e)
{
	g_orig.pfnClientPutInServer(e);
	ForceModel(e, g_eng->pfnGetInfoKeyBuffer(e));
	Log("jogador %d '%s' entrou no mapa", g_eng->pfnIndexOfEdict(e), STRING(e->v.netname));
}

static qboolean Hook_ClientConnect(edict_t* e, const char* name, const char* addr, char reject[128])
{
	qboolean ok = g_orig.pfnClientConnect(e, name, addr, reject);
	Log("conexao do jogador %d '%s' (%s)%s%s", g_eng->pfnIndexOfEdict(e), name, addr, ok ? "" : " recusada: ",
		ok ? "" : reject);
	return ok;
}

// Comandos que o cliente manda (inventario, usar item, trocar arma...). O mesmo comando so volta ao log depois
// de 30 s, com quantas vezes veio nesse meio tempo (o cliente manda "VModEnable 1"/"vban 0" a cada 0,3 s).
static void Hook_ClientCommand(edict_t* e)
{
	int i = g_eng->pfnIndexOfEdict(e);
	const char* name = g_eng->pfnCmd_Argv(0);
	if (i > 0 && i < 33 && !strcmp(name, "cofmp_skip")) // Enter, mandado pela janela do Home de cada jogador
	{
		RequestSkip(i);
		return;
	}
	bool drop = i > 0 && i < 33 && !strcmp(name, "inventorydrop");
	if (i > 0 && i < 33)
	{
		struct Seen
		{
			double at;
			int count;
		};
		static std::unordered_map<std::string, Seen> seen[33];
		std::string cmd = g_eng->pfnCmd_Argv(0);
		const char* args = g_eng->pfnCmd_Args();
		if (args && *args)
			cmd += std::string(" ") + args;
		if (cmd.size() > 200)
			cmd.resize(200);
		Seen& s = seen[i][cmd];
		double now = Plat_Time();
		if (!s.at || now - s.at >= 30.0)
		{
			if (s.count)
				Note("jogador %d: comando '%s' (mais %d vez(es) nos ultimos 30 s)", i, cmd.c_str(), s.count);
			else
				Note("jogador %d: comando '%s'", i, cmd.c_str());
			s.at = now;
			s.count = 0;
		}
		else
			s.count++;
		if (seen[i].size() > 500)
			seen[i].clear();
	}
	if (!drop)
	{
		g_orig.pfnClientCommand(e);
		return;
	}
	DropCommand(e);
}

static void Hook_ClientDisconnect(edict_t* e)
{
	int i = g_eng->pfnIndexOfEdict(e);
	if (i > 0 && i < 33)
	{
		g_arrive.place[i] = false;
		g_wasAlive[i] = false;
		g_unstickAt[i] = 0;
		g_inv[i].pending = false;
		g_inv[i].selectAt = 0;
		g_seenValid[i] = false;
		g_deadSince[i] = 0;
		g_diedHere[i] = false;
		g_aliveLast[i] = false;
		if (g_camOwner == i)
			Hook_SetView(e, e); // devolve a visao dos outros
		g_hold.frozen[i] = false;
		g_cut.waiting[i] = false;
		g_cut.frozen[i] = false;
		g_god[i] = {};
		if (!g_hold.active && !(e->v.flags & FL_FAKECLIENT) && g_humansLast > 0)
			g_humansLast--;
		Log("jogador %d desconectou (foto do inventario %s)", i, g_inv[i].valid ? "mantida" : "vazia");
	}
	g_orig.pfnClientDisconnect(e);
}

// ---------------------------------------------------------------- exports

static bool LoadHL()
{
	if (g_hl)
		return true;
	char path[MAX_PATH];
	HMODULE self;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&LoadHL, &self);
	GetModuleFileNameA(self, path, sizeof(path));
	char* slash = strrchr(path, '\\');
	strcpy(slash ? slash + 1 : path, "hl.dll");
	g_hl = LoadLibraryA(path);
	return g_hl != NULL;
}

static void BuildTable(DLL_FUNCTIONS* out)
{
	*out = g_orig;
	out->pfnGameInit = Hook_GameInit;
	out->pfnKeyValue = Hook_KeyValue;
	out->pfnServerActivate = Hook_ServerActivate;
	out->pfnStartFrame = Hook_StartFrame;
	out->pfnClientDisconnect = Hook_ClientDisconnect;
	out->pfnClientUserInfoChanged = Hook_ClientUserInfoChanged;
	out->pfnClientPutInServer = Hook_ClientPutInServer;
	out->pfnClientConnect = Hook_ClientConnect;
	out->pfnClientCommand = Hook_ClientCommand;
	out->pfnTouch = Hook_Touch;
	out->pfnThink = Hook_Think;
	InstallDetour();
}

// ---------------------------------------------------------------- cutscenes
//
// O trigger_camera do SP so muda a visao de um jogador (o ativador, ou o jogador 1). O hl.dll
// recebe uma copia da tabela do engine com pfnSetView trocado: quando a camera liga para um
// jogador, liga para todos (e congela os outros, como o dono); quando desliga, desliga para todos.

static enginefuncs_t g_engForHL;
static bool g_camShared[33];    // jogadores que estamos mostrando a camera
static edict_t* g_camView;      // camera ligada agora

static void Hook_SetView(const edict_t* client, const edict_t* view)
{
	g_eng->pfnSetView(client, view);
	int i = g_eng->pfnIndexOfEdict(client);
	// durante a troca de mapa o engine derruba bots e limpa os canais: nao mexe na visao de ninguem
	if (!cv_enable.value || i < 1 || i > gpGlobals->maxClients || i >= 33 || gpGlobals->maxClients <= 1 || g_changing)
		return;
	bool on = view != client;
	if (on && (!g_camOwner || g_camOwner == i))
	{
		g_camOwner = i;
		g_camView = (edict_t*)view;
		int n = 0;
		SetGod(i, GOD_CAM, true); // durante a cena ninguem morre
		for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
		{
			edict_t* o = PlayerAt(k);
			if (k == i || !o || !IsAlive(o) || (o->v.flags & FL_FAKECLIENT))
				continue;
			SetGod(k, GOD_CAM, true);
			n++;
			g_eng->pfnSetView(o, view);
			if (client->v.flags & FL_FROZEN)
				o->v.flags |= FL_FROZEN;
			g_camShared[k] = true;
		}
		Log("cutscene: camera '%s' do jogador %d mostrada a mais %d jogador(es)", STRING(view->v.targetname), i, n);
	}
	else if (!on && g_camOwner == i)
	{
		for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
			SetGod(k, GOD_CAM, false);
		for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
		{
			edict_t* o = PlayerAt(k);
			if (!g_camShared[k])
				continue;
			g_camShared[k] = false;
			if (!o)
				continue;
			g_eng->pfnSetView(o, o);
			o->v.flags &= ~FL_FROZEN;
		}
		g_camOwner = 0;
		g_camView = NULL;
		Log("cutscene: camera desligada para todos");
	}
}

// Quem nasce no meio da cena (entrou atrasado, renasceu) passa a ver a mesma camera.
static void CamJoinThink()
{
	if (!g_camOwner || IsNull(g_camView))
		return;
	edict_t* owner = PlayerAt(g_camOwner);
	if (!owner)
		return;
	for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
	{
		edict_t* o = PlayerAt(k);
		if (k == g_camOwner || g_camShared[k] || !o || !IsAlive(o) || (o->v.flags & FL_FAKECLIENT))
			continue;
		g_eng->pfnSetView(o, g_camView);
		if (owner->v.flags & FL_FROZEN)
			o->v.flags |= FL_FROZEN;
		g_camShared[k] = true;
		SetGod(k, GOD_CAM, true);
		Log("cutscene: jogador %d chegou no meio da cena e ve a camera '%s'", k, STRING(g_camView->v.targetname));
	}
}

static void ResetCutscene()
{
	g_camOwner = 0;
	g_camView = NULL;
	memset(g_camShared, 0, sizeof(g_camShared));
}

// ---------------------------------------------------------------- pular cutscene (Enter)
//
// No SP, Enter numa camera (trigger_camera sem a flag 0x400) faz o hl.dll mandar SkipScene ao cliente, que liga
// host_framerate 2: o jogo inteiro anda 2 s por frame ate a camera desligar. No MP nada disso acontece: o hl.dll
// so le o teclado com IsMultiplayer() falso e o Host_FilterTime do engine ignora host_framerate fora do SP.
// Aqui a janela do Home de cada jogador manda "cmd cofmp_skip" no Enter; com uma camera ligada o servidor
// libera o host_framerate no MP (patch no hw.dll) e liga 2 ate a camera desligar.

// Host_FilterTime: if (host_framerate > 0 && (Host_IsSinglePlayerGame() || cls.demoplayback)) -> o "je" final
// pula o host_framerate; com ele trocado por nop, vale tambem no MP.
static const DWORD RVA_FILTER_SP_CHECK = 0x7ea3a;
static const char SIG_FILTER_SP_CHECK[] = "\x85\xC0\x75\x09\x83\x3D????\x00\x74\x2E"; // ? = endereco de cls
static const float SKIP_FRAMERATE = 2.0f; // o valor que o client.dll usa no SP
static const int SF_CAMERA_NOSKIP = 0x400;
static bool g_skipping;

static bool AllowFramerateInMP(bool on)
{
	static BYTE* je;
	static bool tried;
	if (!tried)
	{
		tried = true;
		BYTE* hw = (BYTE*)GetModuleHandleA("hw.dll");
		BYTE* p = hw ? hw + RVA_FILTER_SP_CHECK : NULL;
		if (p && SigMatch(p, SIG_FILTER_SP_CHECK, sizeof(SIG_FILTER_SP_CHECK) - 1))
			je = p + 11;
		else
			Log("ERRO: Host_FilterTime do hw.dll nao confere (outra versao?) - pular cutscene desativado");
	}
	if (!je)
		return false;
	const BYTE want[2] = { BYTE(on ? 0x90 : 0x74), BYTE(on ? 0x90 : 0x2E) };
	DWORD old;
	if (!VirtualProtect(je, 2, PAGE_EXECUTE_READWRITE, &old))
		return false;
	memcpy(je, want, 2);
	VirtualProtect(je, 2, old, &old);
	FlushInstructionCache(GetCurrentProcess(), je, 2);
	return true;
}

static void StopSkip(const char* why)
{
	if (!g_skipping)
		return;
	g_skipping = false;
	g_eng->pfnCVarSetFloat("host_framerate", 0);
	AllowFramerateInMP(false);
	Log("cutscene: fim do pulo (%s)", why);
}

static void RequestSkip(int i)
{
	edict_t* p = PlayerAt(i);
	if (!p || g_changing || g_skipping || !cv_enable.value || gpGlobals->maxClients <= 1)
		return;
	if (!g_camOwner || IsNull(g_camView))
	{
		Note("jogador %d apertou Enter sem cutscene", i);
		return;
	}
	if (g_camView->v.spawnflags & SF_CAMERA_NOSKIP)
	{
		g_eng->pfnClientPrintf(p, print_center, "Esta cena nao pode ser pulada");
		return;
	}
	if (!AllowFramerateInMP(true))
		return;
	g_skipping = true;
	g_eng->pfnCVarSetFloat("host_framerate", SKIP_FRAMERATE);
	Log("cutscene: '%s' pulou a camera '%s'", STRING(p->v.netname), STRING(g_camView->v.targetname));
}

// Cameras encadeadas desligam e ligam no mesmo frame: o pulo segue ate nao sobrar camera.
static void SkipThink()
{
	if (g_skipping && !g_camOwner)
		StopSkip("camera desligou");
}

// ---------------------------------------------------------------- rastreio de itens (diagnostico)
//
// Itens jogados no chao que somem e itens pegos que nao entram no inventario: o log ganha cada mudanca no
// inventario dos jogadores e o destino do que um inventorydrop cria (onde nasceu, quem apagou e quando).

struct Traced
{
	edict_t* e;
	int serial;
	int index;
	float until;
	std::string cls;
};
static std::vector<Traced> g_traced;
static edict_t* g_dropper; // dentro de um inventorydrop: o que o hl.dll criar e o item jogado no chao

static void TraceDrop(edict_t* e)
{
	g_dropper = e;
}

static void TraceAdd(edict_t* e)
{
	if (!g_dropper || !e || g_traced.size() >= 16)
		return;
	g_traced.push_back({ e, e->serialnumber, g_eng->pfnIndexOfEdict(e), gpGlobals->time + 10.0f, "" });
	g_tracing = true;
}

static edict_t* Hook_CreateEntity()
{
	edict_t* e = g_eng->pfnCreateEntity();
	TraceAdd(e);
	return e;
}

static edict_t* Hook_CreateNamedEntity(int className)
{
	edict_t* e = g_eng->pfnCreateNamedEntity(className);
	TraceAdd(e);
	return e;
}

static void Hook_RemoveEntity(edict_t* e)
{
	for (size_t k = 0; k < g_traced.size(); k++)
		if (g_traced[k].e == e && e->serialnumber == g_traced[k].serial)
		{
			Log("item #%d '%s': apagado (REMOVE_ENTITY chamado de hl.dll+0x%x)", g_traced[k].index,
				g_traced[k].cls.empty() ? STRING(e->v.classname) : g_traced[k].cls.c_str(),
				(unsigned)((DWORD)_ReturnAddress() - (DWORD)g_hl));
			g_traced.erase(g_traced.begin() + k);
			break;
		}
	g_tracing = !g_traced.empty();
	g_eng->pfnRemoveEntity(e);
}

static void TraceCheck(const char* during)
{
	for (size_t k = 0; k < g_traced.size();)
	{
		Traced& t = g_traced[k];
		edict_t* e = t.e;
		const char* why = NULL;
		if (e->free || e->serialnumber != t.serial)
			why = "liberado";
		else if (e->v.flags & FL_KILLME)
			why = "marcado para apagar (UTIL_Remove)";
		if (t.cls.empty() && !why)
		{
			// primeira olhada, logo depois do inventorydrop: como o item nasceu
			t.cls = STRING(e->v.classname);
			Log("item #%d '%s' criado no chao: modelo '%s' em %.0f %.0f %.0f, solid %d, movetype %d, effects %d, dono #%d",
				t.index, t.cls.c_str(), STRING(e->v.model), e->v.origin.x, e->v.origin.y, e->v.origin.z, e->v.solid,
				e->v.movetype, e->v.effects, e->v.owner ? g_eng->pfnIndexOfEdict(e->v.owner) : 0);
		}
		if (why)
		{
			Log("item #%d '%s': %s durante %s", t.index, t.cls.empty() ? "?" : t.cls.c_str(), why, during);
			g_traced.erase(g_traced.begin() + k);
			continue;
		}
		k++;
	}
	g_tracing = !g_traced.empty();
}

static void TraceThink()
{
	if (!g_tracing)
		return;
	TraceCheck("o frame (fora de think/touch)");
	for (size_t k = 0; k < g_traced.size();)
	{
		Traced& t = g_traced[k];
		if (gpGlobals->time < t.until)
		{
			k++;
			continue;
		}
		edict_t* e = t.e;
		Log("item #%d '%s' continua no chao depois de 10 s em %.0f %.0f %.0f (effects %d)", t.index, t.cls.c_str(),
			e->v.origin.x, e->v.origin.y, e->v.origin.z, e->v.effects);
		g_traced.erase(g_traced.begin() + k);
	}
	g_tracing = !g_traced.empty();
}

static void ResetTrace()
{
	g_traced.clear();
	g_tracing = false;
	g_dropper = NULL;
}

static std::string SlotsOf(void* pl)
{
	std::string s;
	for (int k = 0; k < 7; k++)
	{
		int str = Field<int>(pl, OFS_INV_SLOT + k * 4);
		s += k ? " " : "";
		s += str && *STRING(str) ? ItemName(STRING(str)) : "-";
	}
	return s;
}

// estado de uma arma/item no log: onde esta, se aparece e o que o hl.dll olha no drop e no pegar
static std::string Describe(edict_t* e)
{
	if (!e)
		return "(nenhum)";
	char buf[400];
	entvars_t* v = &e->v;
	// +0x110 (arquivo do inventario) so existe nas armas; nos outros itens o campo e outra coisa
	int file = e->pvPrivateData && !strncmp(STRING(v->classname), "weapon_", 7) ? Field<int>(e->pvPrivateData, 0x110) : 0;
	_snprintf(buf, sizeof(buf) - 1,
		"#%d '%s' livre %d modelo '%s' (%d) arquivo '%s' effects %d solid %d movetype %d iuser3 %d target '%s' "
		"spawnflags 0x%x flags 0x%x dono #%d aiment #%d em %.0f %.0f %.0f",
		g_eng->pfnIndexOfEdict(e), STRING(v->classname), e->free, v->model ? STRING(v->model) : "", v->modelindex,
		file ? STRING(file) : "",
		v->effects, v->solid, v->movetype, v->iuser3, v->target ? STRING(v->target) : "", v->spawnflags, v->flags,
		v->owner ? g_eng->pfnIndexOfEdict(v->owner) : 0, v->aiment ? g_eng->pfnIndexOfEdict(v->aiment) : 0,
		v->origin.x, v->origin.y, v->origin.z);
	buf[sizeof(buf) - 1] = 0;
	return buf;
}

// inventorydrop N: a arma do slot N antes e depois, e o destino dela nos 10 s seguintes
static void DropCommand(edict_t* e)
{
	int i = g_eng->pfnIndexOfEdict(e), slot = atoi(g_eng->pfnCmd_Argv(1));
	void* pl = e->pvPrivateData;
	edict_t* w = NULL;
	if (pl && slot >= 0 && slot < 7)
	{
		int cls = Field<int>(pl, OFS_INV_CLASS + slot * 4);
		void* it = cls && *STRING(cls) ? FindItem(pl, STRING(cls)) : NULL;
		if (it && PevOf(it))
			w = PevOf(it)->pContainingEntity;
		Log("jogador %d larga o slot %d ('%s'): arma %s", i, slot,
			Field<int>(pl, OFS_INV_SLOT + slot * 4) ? STRING(Field<int>(pl, OFS_INV_SLOT + slot * 4)) : "",
			Describe(w).c_str());
	}
	int serial = w ? w->serialnumber : 0;
	TraceDrop(e); // o que o hl.dll criar agora e o item jogado no chao
	g_orig.pfnClientCommand(e);
	TraceDrop(NULL);
	TraceCheck("o proprio inventorydrop");
	if (!w)
		return;
	if (w->free || w->serialnumber != serial)
	{
		Log("  depois do drop: a arma foi liberada");
		return;
	}
	Log("  depois do drop: %s", Describe(w).c_str());
	if (g_traced.size() < 16)
	{
		g_traced.push_back({ w, serial, g_eng->pfnIndexOfEdict(w), gpGlobals->time + 10.0f, STRING(w->v.classname) });
		g_tracing = true;
	}
}

// jogador encostando numa arma/item: se algo mudou (pegou, nao coube, sumiu), uma linha com o antes e o depois
static bool PickupTouch(edict_t* item, edict_t* player)
{
	if (IsNull(item) || IsNull(player) || !IsPlayer(player) || !player->pvPrivateData || !item->pvPrivateData)
		return false;
	const char* cls = STRING(item->v.classname);
	if (strncmp(cls, "weapon_", 7) && strncmp(cls, "item_", 5) && strncmp(cls, "ammo_", 5))
		return false;
	void* pl = player->pvPrivateData;
	std::string before = Describe(item), slots = SlotsOf(pl);
	int serial = item->serialnumber, effects = item->v.effects, solid = item->v.solid;
	edict_t* owner = item->v.owner;
	g_orig.pfnTouch(item, player);
	bool gone = item->free || item->serialnumber != serial;
	std::string slotsAfter = SlotsOf(pl);
	if (!gone && effects == item->v.effects && solid == item->v.solid && owner == item->v.owner && slots == slotsAfter)
		return true;
	Log("jogador %d pegou %s", g_eng->pfnIndexOfEdict(player), before.c_str());
	Log("  slots [%s] -> [%s]; item depois: %s", slots.c_str(), slotsAfter.c_str(),
		gone ? "liberado" : Describe(item).c_str());
	if (g_tracing)
		TraceCheck("o pegar item");
	return true;
}

// Inventario de cada jogador (os 7 slots e as armas): uma linha no log a cada mudanca.
static void InvWatchThink()
{
	static std::string last[33];
	static float next;
	if (gpGlobals->time < next && gpGlobals->time > next - 1.0f)
		return;
	next = gpGlobals->time + 0.2f;
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* p = PlayerAt(i);
		if (!p || !IsAlive(p) || (p->v.flags & FL_FAKECLIENT))
		{
			last[i].clear();
			continue;
		}
		void* pl = p->pvPrivateData;
		std::string s = "slots:";
		for (int k = 0; k < 7; k++)
		{
			int str = Field<int>(pl, OFS_INV_SLOT + k * 4);
			s += " ";
			s += str && *STRING(str) ? ItemName(STRING(str)) : "-";
		}
		s += " | armas:";
		ForEachItem(pl, [&](void* it) {
			entvars_t* pev = PevOf(it);
			if (pev && pev->classname)
				s += std::string(" ") + STRING(pev->classname);
		});
		if (s != last[i])
		{
			if (!last[i].empty())
				Note("jogador %d inventario %s", i, s.c_str());
			last[i] = s;
		}
	}
}

// ---------------------------------------------------------------- dump de crash
//
// O crashhandler da Steam as vezes morre junto com o jogo e nao salva nada. Na primeira excecao
// fatal (primeira chance, entao pode ser uma que alguem trataria) grava cryoffear\cofmp_crash_*.dmp.

typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);

static LONG CALLBACK CrashDumpHandler(EXCEPTION_POINTERS* ep)
{
	static volatile LONG done;
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	if ((code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
		code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION) || InterlockedExchange(&done, 1))
		return EXCEPTION_CONTINUE_SEARCH;
	HMODULE dbg = LoadLibraryA("dbghelp.dll");
	MiniDumpWriteDump_t write = dbg ? (MiniDumpWriteDump_t)GetProcAddress(dbg, "MiniDumpWriteDump") : NULL;
	if (!write)
		return EXCEPTION_CONTINUE_SEARCH;
	char path[MAX_PATH];
	HMODULE self;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&CrashDumpHandler, &self);
	GetModuleFileNameA(self, path, sizeof(path));
	// ...\cryoffear\cl_dlls\cofmp.dll -> ...\cryoffear\Mod coop\cofmp_crash_*.dmp
	for (int up = 0; up < 2; up++)
		if (char* s = strrchr(path, '\\'))
			*s = 0;
	SYSTEMTIME t;
	GetLocalTime(&t);
	size_t len = strlen(path);
	_snprintf(path + len, sizeof(path) - len, "\\" COFMP_DIR "\\cofmp_crash_%04d%02d%02d_%02d%02d%02d.dmp", t.wYear,
		t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	path[sizeof(path) - 1] = 0;
	HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return EXCEPTION_CONTINUE_SEARCH;
	struct { DWORD tid; EXCEPTION_POINTERS* ep; BOOL client; } info = { GetCurrentThreadId(), ep, FALSE };
	// MiniDumpWithDataSegs (1) | MiniDumpWithIndirectlyReferencedMemory (0x40): globais dos modulos e o que a pilha aponta
	write(GetCurrentProcess(), GetCurrentProcessId(), f, 0x41, &info, NULL, NULL);
	CloseHandle(f);
	return EXCEPTION_CONTINUE_SEARCH;
}

static void HudCommand(const char* cmd)
{
	g_eng->pfnServerCommand((char*)cmd);
}

extern "C" void __stdcall GiveFnptrsToDll(enginefuncs_t* eng, globalvars_t* globals)
{
	g_eng = eng;
	gpGlobals = globals;
	static bool dumpInstalled;
	if (!dumpInstalled)
	{
		dumpInstalled = AddVectoredExceptionHandler(0, CrashDumpHandler) != NULL;
		// log em ...\cryoffear\Mod coop\cofmp_log.txt (a dll fica em cryoffear\cl_dlls)
		char dir[MAX_PATH];
		HMODULE self;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&CrashDumpHandler, &self);
		GetModuleFileNameA(self, dir, sizeof(dir) - sizeof("\\" COFMP_DIR));
		for (int up = 0; up < 2; up++)
			if (char* s = strrchr(dir, '\\'))
				*s = 0;
		strcat(dir, "\\" COFMP_DIR);
		LogFile_Open(dir);
		LogFile_Write("jogo", "---- cofmp v" COFMP_VERSION " carregado");
	}
	Hud_Start(HudCommand);
	if (!LoadHL())
		return;
	g_engForHL = *eng;
	g_engForHL.pfnSetView = Hook_SetView;
	g_engForHL.pfnCreateEntity = Hook_CreateEntity;
	g_engForHL.pfnCreateNamedEntity = Hook_CreateNamedEntity;
	g_engForHL.pfnRemoveEntity = Hook_RemoveEntity;
	typedef void(__stdcall * fn_t)(enginefuncs_t*, globalvars_t*);
	((fn_t)GetProcAddress(g_hl, "GiveFnptrsToDll"))(&g_engForHL, globals);
}

extern "C" int GetEntityAPI2(DLL_FUNCTIONS* table, int* version)
{
	if (!LoadHL())
		return 0;
	typedef int (*fn_t)(DLL_FUNCTIONS*, int*);
	if (!((fn_t)GetProcAddress(g_hl, "GetEntityAPI2"))(&g_orig, version))
		return 0;
	BuildTable(table);
	return 1;
}

extern "C" int GetEntityAPI(DLL_FUNCTIONS* table, int version)
{
	if (!LoadHL())
		return 0;
	typedef int (*fn_t)(DLL_FUNCTIONS*, int);
	if (!((fn_t)GetProcAddress(g_hl, "GetEntityAPI"))(&g_orig, version))
		return 0;
	BuildTable(table);
	return 1;
}
