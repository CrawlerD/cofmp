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
#include "studio.h"
#include <intrin.h>
#include <psapi.h>
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
static cvar_t cv_selfrespawn_time = { (char*)"cofmp_selfrespawn_time", (char*)"10", FCVAR_SERVER }; // pulo so renasce depois disso
static cvar_t cv_model = { (char*)"cofmp_model", (char*)"simon", FCVAR_SERVER };
static cvar_t cv_start_wait = { (char*)"cofmp_start_wait", (char*)"1", FCVAR_SERVER };
static cvar_t cv_start_timeout = { (char*)"cofmp_start_timeout", (char*)"90", FCVAR_SERVER };
static cvar_t cv_cutscene_wait = { (char*)"cofmp_cutscene_wait", (char*)"1", FCVAR_SERVER };
static cvar_t cv_touchlog = { (char*)"cofmp_touchlog", (char*)"0", FCVAR_SERVER }; // diagnostico: toques jogador x trigger_*

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
	if (tr.fStartSolid || tr.fAllSolid)
		return false;
	// o trace nem sempre ve outro jogador (o segundo chegou em cima do primeiro no c_start2)
	for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
	{
		edict_t* o = g_eng->pfnPEntityOfEntIndex(k);
		if (IsNull(o) || o == skip || !(o->v.flags & FL_CLIENT) || !o->pvPrivateData || !IsAlive(o) ||
			o->v.solid == SOLID_NOT || o->v.iuser1) // quem espia tem o corpo junto do seguido
			continue;
		Vector d = o->v.origin - pos;
		if (fabsf(d.x) < 33 && fabsf(d.y) < 33 && fabsf(d.z) < 73)
			return false;
	}
	return true;
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
static float g_skipEndAt = -1000; // quando o ultimo pulo de cutscene acabou
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
	OFS_FLASH_FLAGS = 0x5d0,  // lanterna ligada (mandado ao cliente pela mensagem FlashFlags)
	OFS_VIEW_ENTITY = 0x2038, // indice da entidade pela qual o jogador olha (mensagem ViewEntity)
	OFS_GOOD_POINTS = 0x5f0,
	OFS_ITEM_FILE = 0x110,   // string_t arquivo do inventario da arma
	OFS_ITEM_PLAYER = 0x160, // CBasePlayer* m_pPlayer
	OFS_ITEM_NEXT = 0x164,   // CBasePlayerItem* m_pNext
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

// chaveiro do grupo (ShareThink, mais abaixo)
static std::vector<std::string> g_keyring;
static std::vector<std::string> g_seen[33]; // itens de cada jogador na ultima varredura
static bool g_seenValid[33];
static float g_nextShare;
static float g_giveRetryAt[33];

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
	std::string slot[7], slotModel[7], slotClass[7]; // inventorySlot* exatos ("" = vazio)
	bool afterDeath; // devolver apos morte/revive: refaz os slots exatamente como na foto
} g_inv[33];

static std::string StrOrEmpty(int str)
{
	return str ? STRING(str) : "";
}

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
	s.afterDeath = false;
	for (int k = 0; k < 7; k++)
	{
		s.slot[k] = StrOrEmpty(Field<int>(pl, OFS_INV_SLOT + k * 4));
		s.slotModel[k] = StrOrEmpty(Field<int>(pl, OFS_INV_MODEL + k * 4));
		s.slotClass[k] = StrOrEmpty(Field<int>(pl, OFS_INV_CLASS + k * 4));
	}
	entvars_t* act = PevOf(Field<void*>(pl, OFS_ACTIVE_ITEM));
	s.active = act ? STRING(act->classname) : "";
	memcpy(s.ammo, &Field<int>(pl, OFS_AMMO), sizeof(s.ammo));
	s.flashBattery = Field<int>(pl, OFS_FLASH_BATTERY);
	s.goodPoints = Field<int>(pl, OFS_GOOD_POINTS);
	if (!quiet)
		Log("inventario do jogador %d salvo: %d arma(s), %d item(ns), ativa '%s'", i, (int)s.weapons.size(),
		(int)s.items.size(), s.active.c_str());
}

static void RestoreAfterDeath(int i, edict_t* p);

static void RestoreInventory(int i, edict_t* p)
{
	SavedInv& s = g_inv[i];
	if (s.afterDeath)
	{
		RestoreAfterDeath(i, p);
		return;
	}
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
//  - reviver nao precisa de item: Usar perto do caido (ReviveUseThink);
//  - ninguem renasce sozinho antes de cofmp_selfrespawn_time (10 s): o grupo tem tempo de reviver;
//  - se o caido apertar pular depois disso (ou passar cofmp_respawn_time sem revive), renasce perto de um
//    companheiro vivo com os itens da ultima foto do inventario, em vez de perder as armas;
//  - quem volta a vida (revive ou respawn nativo) recebe de volta o que faltar da foto, e se o
//    respawn nativo o jogou longe, e levado para perto do grupo.
// A foto e tirada a cada segundo. Se todos morrem, o mapa reinicia e cada um recebe a foto.

static void CenterPrintAll(const char* msg);

typedef void(__thiscall* StartRevive_t)(void* player);
static StartRevive_t g_StartRevive;
static const DWORD RVA_StartRevive = 0xf1860;
static const BYTE SIG_StartRevive[] = { 0x56, 0x8B, 0xF1, 0x57, 0xC6, 0x86, 0x0C, 0x05, 0x00, 0x00, 0x00, 0xC7 };

// Sai da camera em 3a pessoa da morte (manda ThirdPerson 0 e zera a flag), como o fim do revive faz.
typedef void(__thiscall* ExitThirdPerson_t)(void* player);
static ExitThirdPerson_t g_ExitThirdPerson;
static const DWORD RVA_ExitThirdPerson = 0xf67f0;
static const BYTE SIG_ExitThirdPerson[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x04, 0xFF, 0xB0, 0x0C, 0x02, 0x00, 0x00 };

// Manda ao cliente o conteudo de um slot do inventario (o terceiro argumento nao e usado).
typedef void(__thiscall* SendInvSlot_t)(void* player, int slot, int file, int unused);
static SendInvSlot_t g_SendInvSlot;
static const DWORD RVA_SendInvSlot = 0xe4360;
static const BYTE SIG_SendInvSlot[] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0xF1, 0x33, 0xDB, 0x8B, 0x46, 0x04 };
static float g_resendAt[33];

// O Spawn limpa a tela de inventario do cliente, mas os slots continuam no servidor: so a arma na mao
// voltava. Reenvia cada slot (depois do ResetHUD do spawn) e tira a camera da 3a pessoa.
static void ResendInventoryThink()
{
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		if (!g_resendAt[i] || gpGlobals->time < g_resendAt[i])
			continue;
		g_resendAt[i] = 0;
		edict_t* p = PlayerAt(i);
		if (!p || !IsAlive(p))
			continue;
		void* pl = p->pvPrivateData;
		if (g_ExitThirdPerson)
			g_ExitThirdPerson(pl);
		if (!g_SendInvSlot)
			continue;
		int n = 0;
		for (int k = 1; k < 7; k++)
		{
			int str = Field<int>(pl, OFS_INV_SLOT + k * 4);
			g_SendInvSlot(pl, k, str, 0);
			n += str && *STRING(str);
		}
		Log("jogador %d: inventario reenviado ao cliente (%d slot(s) ocupado(s))", i, n);
	}
}
static const float REVIVING = 3.0f; // pev->fuser3 durante a animacao de revive

static float g_deadSince[33];

// Armas que o jogador tinha ao morrer. A morte zera os slots do inventario mas mantem as armas; o Spawn
// do renascer da uma pistola nova. Arma que nao estava na foto nem com o jogador na morte e dessas.
static std::vector<std::pair<edict_t*, int>> g_deathWeapons[33];

static void RememberDeathWeapons(int i, void* pl)
{
	g_deathWeapons[i].clear();
	ForEachItem(pl, [&](void* it) {
		entvars_t* pev = PevOf(it);
		if (pev && pev->pContainingEntity)
			g_deathWeapons[i].push_back({ pev->pContainingEntity, pev->pContainingEntity->serialnumber });
	});
}

static bool HadAtDeath(int i, void* it)
{
	entvars_t* pev = PevOf(it);
	edict_t* e = pev ? pev->pContainingEntity : nullptr;
	for (const auto& w : g_deathWeapons[i])
		if (w.first == e && w.second == e->serialnumber)
			return true;
	return false;
}

enum
{
	OFS_CLIENT_ACTIVE_ITEM = 0x2530, // m_pClientActiveItem
	OFS_SWITCH_ITEM = 0x2538,        // arma que o SelectLastItem vai sacar
};

// Tira a arma do jogador (listas, arma na mao) e apaga a entidade, sem largar nada no chao.
static void RemoveWeapon(void* pl, void* it)
{
	for (int s = 0; s < 6; s++)
	{
		void** link = &Field<void*>(pl, OFS_PLAYER_ITEMS + s * 4);
		for (int guard = 0; *link && guard < 64; guard++)
		{
			if (*link == it)
			{
				*link = Field<void*>(it, OFS_ITEM_NEXT);
				break;
			}
			link = &Field<void*>(*link, OFS_ITEM_NEXT);
		}
	}
	entvars_t* ppev = PevOf(pl);
	if (Field<void*>(pl, OFS_ACTIVE_ITEM) == it)
	{
		Field<void*>(pl, OFS_ACTIVE_ITEM) = nullptr;
		ppev->viewmodel = 0;
		ppev->weaponmodel = 0;
	}
	if (Field<void*>(pl, OFS_CLIENT_ACTIVE_ITEM) == it)
		Field<void*>(pl, OFS_CLIENT_ACTIVE_ITEM) = nullptr;
	if (Field<void*>(pl, OFS_LAST_ITEM) == it)
		Field<void*>(pl, OFS_LAST_ITEM) = nullptr;
	if (Field<void*>(pl, OFS_SWITCH_ITEM) == it)
		Field<void*>(pl, OFS_SWITCH_ITEM) = nullptr;
	int id = Field<int>(it, OFS_ITEM_ID);
	if (id > 0 && id < 32)
		ppev->weapons &= ~(1 << id);
	Field<void*>(it, OFS_ITEM_PLAYER) = nullptr;
	entvars_t* w = PevOf(it);
	w->effects |= EF_NODRAW;
	w->flags |= FL_KILLME;
}

// Depois de morrer (renascer ou revive): devolve as armas que faltam, apaga a pistola extra do Spawn e
// regrava os 7 slots exatamente como na foto, na mesma posicao (o atalho do quick select continua certo).
// Item que o grupo gastou enquanto o jogador estava caido nao volta.
static void RestoreAfterDeath(int i, edict_t* p)
{
	SavedInv& s = g_inv[i];
	void* pl = p->pvPrivateData;
	s.afterDeath = false;
	int before[AMMO_SLOTS];
	memcpy(before, &Field<int>(pl, OFS_AMMO), sizeof(before));

	int given = 0, removed = 0;
	for (const SavedWeapon& w : s.weapons)
	{
		void* it = FindItem(pl, w.cls.c_str());
		if (it)
		{
			Field<int>(it, OFS_ITEM_CLIP) = max(Field<int>(it, OFS_ITEM_CLIP), w.clip);
			continue;
		}
		g_GiveNamedItem(pl, STRING(EngString(w.cls)), false, true);
		it = FindItem(pl, w.cls.c_str());
		if (it)
		{
			Field<int>(it, OFS_ITEM_CLIP) = w.clip;
			given++;
		}
		else
			Log("jogador %d: nao consegui devolver '%s'", i, w.cls.c_str());
	}

	std::vector<void*> extra;
	ForEachItem(pl, [&](void* it) {
		entvars_t* pev = PevOf(it);
		if (!pev || HadAtDeath(i, it))
			return;
		for (const SavedWeapon& w : s.weapons)
			if (w.cls == STRING(pev->classname))
				return;
		extra.push_back(it);
	});
	for (void* it : extra)
	{
		Log("jogador %d: tirando '%s' que o renascer deu a mais", i, STRING(PevOf(it)->classname));
		RemoveWeapon(pl, it);
		removed++;
	}

	std::string slots;
	for (int k = 0; k < 7; k++)
	{
		std::string f = s.slot[k];
		if (!f.empty() && IsWeaponFile(f.c_str()))
		{
			size_t a = f.find_last_of('/'), b = f.rfind(".txt");
			std::string cls = f.substr(a + 1, b == std::string::npos || b <= a ? std::string::npos : b - a - 1);
			if (!FindItem(pl, cls.c_str()))
				f.clear();
		}
		else if (!f.empty() && cv_share_items.value && !Has(g_keyring, f))
			f.clear(); // gasto pelo grupo
		Field<int>(pl, OFS_INV_SLOT + k * 4) = f.empty() ? 0 : EngString(f);
		Field<int>(pl, OFS_INV_MODEL + k * 4) = f.empty() || s.slotModel[k].empty() ? 0 : EngString(s.slotModel[k]);
		Field<int>(pl, OFS_INV_CLASS + k * 4) = f.empty() || s.slotClass[k].empty() ? 0 : EngString(s.slotClass[k]);
		if (k && g_SendInvSlot)
			g_SendInvSlot(pl, k, Field<int>(pl, OFS_INV_SLOT + k * 4), 0);
		slots += f.empty() ? " -" : " " + ItemName(f);
	}
	g_seenValid[i] = false; // os slots mudaram por fora: o ShareThink nao pode achar que algo foi gasto

	int* ammo = &Field<int>(pl, OFS_AMMO);
	for (int k = 0; k < AMMO_SLOTS; k++)
		ammo[k] = max(before[k], s.ammo[k]);
	Field<int>(pl, OFS_FLASH_BATTERY) = s.flashBattery;
	Field<int>(pl, OFS_GOOD_POINTS) = s.goodPoints;
	Log("inventario do jogador %d refeito depois da morte: [%s ] (%d arma(s) devolvida(s), %d a mais tirada(s))", i,
		slots.c_str(), given, removed);
}

// ---- corpo no chao
// O Killed do jogo poe a sequencia "death" (ou "crouch_death"), mas o corpo ficava de pe nas duas telas.
// Enquanto o jogador esta caido o mod mantem a sequencia de morte com o quadro calculado pelo tempo desde a
// morte; com framerate 1 o cliente anda os quadros entre um pacote e outro e para no ultimo (deitado).
static int g_deathSeq[33];
static float g_deathFps[33];
static int g_deathFrames[33];
static int g_poseLog[33];

static int FindSequence(edict_t* e, const char* name, float* fps, int* frames)
{
	studiohdr_t* h = (studiohdr_t*)g_eng->pfnGetModelPtr(e);
	if (!h)
		return -1;
	mstudioseqdesc_t* sq = (mstudioseqdesc_t*)((byte*)h + h->seqindex);
	for (int k = 0; k < h->numseq; k++)
		if (!_stricmp(sq[k].label, name))
		{
			*fps = sq[k].fps;
			*frames = sq[k].numframes;
			return k;
		}
	return -1;
}

static void StartDeathPose(int i, edict_t* p)
{
	const char* anim = (p->v.flags & FL_DUCKING) ? "crouch_death" : "death";
	g_deathSeq[i] = FindSequence(p, anim, &g_deathFps[i], &g_deathFrames[i]);
	if (g_deathSeq[i] < 0)
		g_deathSeq[i] = FindSequence(p, anim = "death", &g_deathFps[i], &g_deathFrames[i]);
	g_poseLog[i] = 0;
	Log("jogador %d caiu: sequencia %d frame %.0f framerate %.2f gait %d -> '%s' (#%d, %d quadros a %.0f fps)", i,
		p->v.sequence, p->v.frame, p->v.framerate, p->v.gaitsequence, anim, g_deathSeq[i], g_deathFrames[i], g_deathFps[i]);
}

static void DeathPoseThink()
{
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* p = PlayerAt(i);
		if (!p || IsAlive(p) || !g_deadSince[i] || g_deathSeq[i] < 0 || p->v.fuser3 == REVIVING)
			continue;
		if (p->v.sequence != g_deathSeq[i] && g_poseLog[i]++ < 3)
			Log("jogador %d caido: o jogo trocou a sequencia do corpo para %d (frame %.0f framerate %.2f)", i,
				p->v.sequence, p->v.frame, p->v.framerate);
		float t = gpGlobals->time - g_deadSince[i];
		float frame = g_deathFrames[i] > 1 ? t * g_deathFps[i] * 256.0f / (g_deathFrames[i] - 1) : 255.0f;
		p->v.sequence = g_deathSeq[i];
		p->v.gaitsequence = 0;
		p->v.frame = min(frame, 255.0f);
		p->v.animtime = gpGlobals->time;
		p->v.framerate = 1.0f;
	}
}

// ---- reviver sem item
// O CoF tem RCP nativo: CBasePlayer::Use no caido faz o vivo se ajoelhar (cpr_down/cpr_idle) e, segurando
// Usar, chama o StartRevive. Mas o Usar do jogo raramente acha o corpo. Aqui: apertou Usar perto de um
// amigo caido, olhando para ele, o mod chama esse Use; se o jogo recusar, chama o StartRevive direto.
static const int OFS_CPR_TARGET = 0x2698; // EHANDLE do caido que o jogador esta reanimando
static bool g_useLast[33];

static void ReviveUseThink()
{
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* r = PlayerAt(i);
		bool use = r && IsAlive(r) && (r->v.button & IN_USE);
		bool pressed = use && !g_useLast[i];
		g_useLast[i] = use;
		if (!pressed)
			continue;
		g_eng->pfnMakeVectors(r->v.v_angle);
		Vector fwd = gpGlobals->v_forward;
		fwd.z = 0;
		edict_t* best = nullptr;
		float bestDist = 1e9f;
		for (int j = 1; j <= gpGlobals->maxClients && j < 33; j++)
		{
			edict_t* o = PlayerAt(j);
			if (j == i || !o || IsAlive(o) || !g_deadSince[j] || o->v.fuser3 == REVIVING)
				continue;
			Vector d = o->v.origin - r->v.origin;
			float dz = fabsf(d.z);
			d.z = 0;
			float dist = d.Length();
			if (dist > 110.0f || dz > 72.0f)
				continue;
			if (dist > 40.0f && DotProduct(d * (1.0f / dist), fwd.Normalize()) < 0.5f)
				continue;
			if (dist < bestDist)
				bestDist = dist, best = o;
		}
		if (!best)
			continue;
		void* rp = r->pvPrivateData;
		if (Field<edict_t*>(rp, OFS_CPR_TARGET) == best)
			continue; // o Usar do jogo ja comecou
		g_orig.pfnUse(best, r);
		if (Field<edict_t*>(rp, OFS_CPR_TARGET) == best)
			Log("jogador %d comecou a reanimar o jogador %d (RCP do jogo)", i, g_eng->pfnIndexOfEdict(best));
		else if (g_StartRevive)
		{
			g_StartRevive(best->pvPrivateData);
			Log("jogador %d levantou o jogador %d (o RCP do jogo recusou)", i, g_eng->pfnIndexOfEdict(best));
		}
	}
}
static bool g_hinted[33];   // ja avisou que pode pular para renascer
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
		g_inv[i].afterDeath = true;
	}
	g_seenValid[i] = false; // o ShareThink devolve as chaves do grupo
}

static void RespawnNear(int i, edict_t* p, edict_t* buddy)
{
	p->v.deadflag = DEAD_NO;
	p->v.iuser1 = 0;
	p->v.fuser3 = 0;
	g_orig.pfnSpawn(p); // CBasePlayer::Spawn
	if (g_ExitThirdPerson)
		g_ExitThirdPerson(p->pvPrivateData); // o Spawn nao sai da 3a pessoa da morte
	g_resendAt[i] = gpGlobals->time + 1.0f;
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
				g_resendAt[i] = gpGlobals->time + 1.0f;
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
			g_hinted[i] = false;
			RememberDeathWeapons(i, p->pvPrivateData);
			StartDeathPose(i, p);
			char msg[192];
			_snprintf(msg, sizeof(msg), "Um amigo pode te levantar segurando USAR perto de voce.\nEm %.0f s voce pode pular para renascer perto do grupo.",
				cv_selfrespawn_time.value);
			msg[sizeof(msg) - 1] = 0;
			g_eng->pfnClientPrintf(p, print_center, msg);
			continue;
		}
		if (!g_deadSince[i] || p->v.fuser3 == REVIVING)
			continue; // sendo revivido: nao interrompe a animacao
		float waited = gpGlobals->time - g_deadSince[i];
		bool canJump = waited >= cv_selfrespawn_time.value;
		if (canJump && !g_hinted[i])
		{
			g_hinted[i] = true;
			g_eng->pfnClientPrintf(p, print_center, "Pule para renascer perto do grupo (com seus itens)");
		}
		bool jump = (p->v.button & IN_JUMP) && canJump;
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
	memset(g_resendAt, 0, sizeof(g_resendAt));
	memset(g_hinted, 0, sizeof(g_hinted));
	memset(g_useLast, 0, sizeof(g_useLast));
	for (int i = 0; i < 33; i++)
		g_deathWeapons[i].clear(); // as entidades morreram com o mapa
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

// O cof_teleport leva so o ativador. No fim da abertura do c_intro ele poe o jogador 1 dentro da saida para o
// c_nightmare, e o resto do grupo nunca chega la. Saida tocada logo depois de um cof_teleport e roteiro do mapa,
// nao alguem andando ate ela: troca na hora, e o DoChangeLevel leva quem ficou longe para o lado do teleportado.
static float g_scriptTeleportAt = -1000.0f;

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
	bool scripted = gpGlobals->time - g_scriptTeleportAt < 1.0f;
	if (scripted)
		Log("saida tocada logo depois de um cof_teleport: troca sem esperar o grupo");
	if (!touch || !cv_exit_wait.value || scripted)
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

// ---------------------------------------------------------------- jogador "atual"
//
// Muitas coisas do CoF agem no "primeiro jogador" (UTIL_FindEntityByClassname(NULL, "player")), que no MP e sempre o
// host: o padlockrotate do convidado girava o cadeado do host e travava os dois (o mesmo vale para o computador, o
// telefone, o telescopio...). O hl.dll recebe pfnFindEntityByString trocado: enquanto o servidor processa algo de um
// jogador (comando, think, toque), a busca por "player" devolve ele primeiro e os outros depois.
static edict_t* g_actor;
static edict_t* g_ownView[33]; // visao de um jogador so (cadeado, zoom, telescopio...), fora das cutscenes

struct ActorScope
{
	edict_t* old;
	explicit ActorScope(edict_t* e) : old(g_actor)
	{
		if (e && (e->v.flags & FL_CLIENT) && cv_enable.value && gpGlobals->maxClients > 1)
			g_actor = e;
	}
	~ActorScope() { g_actor = old; }
};

static edict_t* Hook_FindEntityByString(edict_t* start, const char* field, const char* value)
{
	edict_t* a = g_actor;
	if (!a || a->free || !field || !value || strcmp(field, "classname") || strcmp(value, "player"))
		return g_eng->pfnFindEntityByString(start, field, value);
	if (IsNull(start))
		return a;
	edict_t* e = g_eng->pfnFindEntityByString(start == a ? NULL : start, field, value);
	if (e == a)
		e = g_eng->pfnFindEntityByString(e, field, value);
	return e;
}

// ---------------------------------------------------------------- objetivo e notas para todos
//
// O cof_objective grava o objetivo so no primeiro jogador (o host): o convidado nunca via. Cada jogador recebe o
// mesmo (o jogador manda a mensagem Objectives sozinho quando o texto muda). As notas (documentos lidos, codigos)
// ficam num vetor de 16 entradas em cada jogador: o que um pega e copiado para os outros e mandado ao cliente
// com a mesma mensagem AddNote que o jogo usa ao reenviar a lista.

static const int OFS_OBJ_TARGET = 0x420, OFS_OBJ_MSG = 0x1e90; // string_t
static const int EV_TARGET = 0x1cc, EV_MESSAGE = 0x1d8;         // entvars_t
static bool g_objSet;
static int g_objTarget, g_objMsg;

static void ObjectiveThink()
{
	if (!g_objSet)
		return;
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* p = PlayerAt(i);
		if (!p || (p->v.flags & FL_FAKECLIENT))
			continue;
		void* pl = p->pvPrivateData;
		if (Field<int>(pl, OFS_OBJ_MSG) == g_objMsg && (!g_objTarget || Field<int>(pl, OFS_OBJ_TARGET) == g_objTarget))
			continue;
		if (g_objTarget)
			Field<int>(pl, OFS_OBJ_TARGET) = g_objTarget;
		Field<int>(pl, OFS_OBJ_MSG) = g_objMsg;
	}
}

static const int NOTE_BASE = 0x688, NOTE_SIZE = 0x120, NOTE_MAX = 16; // entrada n (1..16) em NOTE_BASE + n*NOTE_SIZE
static const int RVA_MSG_ADDNOTE = 0x226648;                         // int gmsgAddNote
static const int MSG_ONE_ = 1;
// campos de texto de uma entrada, na ordem da mensagem AddNote (o primeiro diz se a entrada esta em uso)
static const struct { int ofs, size; } g_noteFields[] = {
	{ 0x40, 16 }, { 0x50, 16 }, { 0x60, 16 }, { 0x70, 16 }, { 0x00, 16 }, { 0x10, 16 },
	{ 0x20, 16 }, { 0x30, 16 }, { 0x80, 0x30 }, { 0xb0, 0x40 }, { 0xf0, 0x30 },
};

static BYTE* NoteAt(void* pl, int n)
{
	return (BYTE*)pl + NOTE_BASE + n * NOTE_SIZE;
}

static bool SameNote(const BYTE* a, const BYTE* b)
{
	for (const auto& f : g_noteFields)
		if (strncmp((const char*)a + f.ofs, (const char*)b + f.ofs, f.size))
			return false;
	return true;
}

static bool HasNote(void* pl, const BYTE* note)
{
	for (int n = 1; n <= NOTE_MAX; n++)
		if (NoteAt(pl, n)[0x40] && SameNote(NoteAt(pl, n), note))
			return true;
	return false;
}

static void SendNote(edict_t* p, int n)
{
	int msg = *(int*)((BYTE*)g_hl + RVA_MSG_ADDNOTE);
	if (msg <= 0)
		return;
	const BYTE* e = NoteAt(p->pvPrivateData, n);
	g_eng->pfnMessageBegin(MSG_ONE_, msg, NULL, p);
	g_eng->pfnWriteByte(n);
	g_eng->pfnWriteByte(0);
	g_eng->pfnWriteShort(0);
	g_eng->pfnWriteByte(0);
	for (const auto& f : g_noteFields)
	{
		char s[0x41];
		memcpy(s, e + f.ofs, f.size);
		s[f.size] = 0;
		g_eng->pfnWriteString(s);
	}
	g_eng->pfnMessageEnd();
}

static void NotesThink()
{
	static float next;
	if (gpGlobals->time < next && next - gpGlobals->time < 1.0f)
		return;
	next = gpGlobals->time + 0.5f;
	for (int a = 1; a <= gpGlobals->maxClients && a < 33; a++)
	{
		edict_t* pa = PlayerAt(a);
		if (!pa || (pa->v.flags & FL_FAKECLIENT))
			continue;
		for (int n = 1; n <= NOTE_MAX; n++)
		{
			const BYTE* note = NoteAt(pa->pvPrivateData, n);
			if (!note[0x40])
				continue;
			for (int b = 1; b <= gpGlobals->maxClients && b < 33; b++)
			{
				edict_t* pb = PlayerAt(b);
				if (b == a || !pb || !IsAlive(pb) || (pb->v.flags & FL_FAKECLIENT) || HasNote(pb->pvPrivateData, note))
					continue;
				int m = 1;
				while (m <= NOTE_MAX && NoteAt(pb->pvPrivateData, m)[0x40])
					m++;
				if (m > NOTE_MAX)
					continue;
				memcpy(NoteAt(pb->pvPrivateData, m), note, NOTE_SIZE);
				SendNote(pb, m);
				char msg[128];
				_snprintf(msg, sizeof(msg), "Nota nova nas suas notas (achada por %s)", STRING(pa->v.netname));
				msg[sizeof(msg) - 1] = 0;
				g_eng->pfnClientPrintf(pb, print_center, msg);
				Log("nota %d do jogador %d ('%.16s') copiada para o jogador %d (entrada %d)", n, a,
					(const char*)note + 0x40, b, m);
			}
		}
	}
}

// inventorydualwield: o ClientCommand do hl.dll ignora no MP; chamamos o metodo do jogador direto
typedef void(__thiscall* DualWield_t)(void* player, int a, int b);
static DualWield_t g_DualWield;
static const DWORD RVA_DualWield = 0xde990;
static const BYTE SIG_DualWield[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x1C, 0x01, 0x00, 0x00 };

static const DWORD RVA_ChangeLevelNow = 0x109d40;
static const BYTE SIG_ChangeLevelNow[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x8B, 0x45, 0x08, 0x53, 0x8B, 0xD9 };
static const int STOLEN = 6; // push ebp / mov ebp,esp / sub esp,8

// Entidades do CoF que agem so no ativador: o cof_teleport (leva so quem disparou a cena - no comeco do c_nightmare o
// convidado ficava no quarto escuro), o cof_clearitems e o cof_giveitems (o mapa tira e da itens, como o celular e o
// canivete no c_start). O Use de cada uma e trocado na vtable: roda normal e depois de novo para cada outro jogador
// vivo, como se ele tambem tivesse disparado.
typedef void(__thiscall* Use_t)(void* self, void* pActivator, void* pCaller, int useType, float value);
static const int VT_USE = 58;
enum { EACH_TELEPORT, EACH_CLEARITEMS, EACH_GIVEITEMS, EACH_OBJECTIVE, EACH_COUNT };
static struct
{
	const char* cls;
	DWORD vt, use; // RVAs
	Use_t orig;
} g_each[EACH_COUNT] = {
	{ "cof_teleport", 0x1c5b50, 0xa7990 },
	{ "cof_clearitems", 0x1c3cd0, 0xa60c0 },
	{ "cof_giveitems", 0x1c49b4, 0xa6bc0 },
	{ "cof_objective", 0x1c985c, 0xa4b00 },
};

static void UseForEach(int k, void* self, void* pActivator, void* pCaller, int useType, float value)
{
	g_each[k].orig(self, pActivator, pCaller, useType, value);
	entvars_t* act = pActivator ? *(entvars_t**)((BYTE*)pActivator + 4) : NULL;
	if (!act || !(act->flags & FL_CLIENT) || gpGlobals->maxClients <= 1 || !cv_enable.value)
		return;
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* o = PlayerAt(i);
		if (!o || o == act->pContainingEntity || !IsAlive(o) || !o->pvPrivateData)
			continue;
		g_each[k].orig(self, o->pvPrivateData, pCaller, useType, value);
		if (k == EACH_TELEPORT)
			g_unstickAt[i] = gpGlobals->time + 0.2f; // caiu em cima de quem foi antes
	}
	edict_t* ent = (*(entvars_t**)((BYTE*)self + 4))->pContainingEntity;
	Log("%s '%s' (%s) repetido para todos os jogadores", g_each[k].cls, STRING(ent->v.targetname),
		STRING(ent->v.message));
}

static void __fastcall Hook_TeleportUse(void* self, void* /*edx*/, void* pActivator, void* pCaller, int useType, float value)
{
	g_scriptTeleportAt = gpGlobals->time;
	UseForEach(EACH_TELEPORT, self, pActivator, pCaller, useType, value);
}

static void __fastcall Hook_ClearItemsUse(void* self, void*, void* pActivator, void* pCaller, int useType, float value)
{
	UseForEach(EACH_CLEARITEMS, self, pActivator, pCaller, useType, value);
}

static void __fastcall Hook_GiveItemsUse(void* self, void*, void* pActivator, void* pCaller, int useType, float value)
{
	entvars_t* pev = *(entvars_t**)((BYTE*)self + 4);
	if ((pev->spawnflags & 1) && cv_share_items.value) // item de inventario (chave etc.): o ShareThink ja divide
		g_each[EACH_GIVEITEMS].orig(self, pActivator, pCaller, useType, value);
	else
		UseForEach(EACH_GIVEITEMS, self, pActivator, pCaller, useType, value);
}

static void __fastcall Hook_ObjectiveUse(void* self, void*, void* pActivator, void* pCaller, int useType, float value)
{
	g_each[EACH_OBJECTIVE].orig(self, pActivator, pCaller, useType, value);
	if (gpGlobals->maxClients <= 1 || !cv_enable.value)
		return;
	BYTE* pev = *(BYTE**)((BYTE*)self + 4);
	if (*(int*)(pev + EV_TARGET))
		g_objTarget = *(int*)(pev + EV_TARGET);
	g_objMsg = *(int*)(pev + EV_MESSAGE);
	g_objSet = true;
	ObjectiveThink();
	Log("objetivo '%s' para todos os jogadores", g_objTarget ? STRING(g_objTarget) : "");
}

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
	BYTE* tp = (BYTE*)g_hl + RVA_ExitThirdPerson;
	if (!memcmp(tp, SIG_ExitThirdPerson, sizeof(SIG_ExitThirdPerson)))
		g_ExitThirdPerson = (ExitThirdPerson_t)tp;
	else
		Log("aviso: assinatura de ExitThirdPerson nao confere - camera pode ficar em 3a pessoa ao renascer");
	BYTE* slot = (BYTE*)g_hl + RVA_SendInvSlot;
	if (!memcmp(slot, SIG_SendInvSlot, sizeof(SIG_SendInvSlot)))
		g_SendInvSlot = (SendInvSlot_t)slot;
	else
		Log("aviso: assinatura de SendInvSlot nao confere - inventario pode sumir da tela ao renascer");

	BYTE* dw = (BYTE*)g_hl + RVA_DualWield;
	if (!memcmp(dw, SIG_DualWield, sizeof(SIG_DualWield)))
		g_DualWield = (DualWield_t)dw;
	else
		Log("aviso: assinatura de DualWield nao confere - empunhar duas armas fica como o jogo deixa no MP");

	void* hooks[EACH_COUNT] = { (void*)&Hook_TeleportUse, (void*)&Hook_ClearItemsUse, (void*)&Hook_GiveItemsUse,
		(void*)&Hook_ObjectiveUse };
	for (int k = 0; k < EACH_COUNT; k++)
	{
		void** vt = (void**)((BYTE*)g_hl + g_each[k].vt);
		DWORD vold;
		if (vt[VT_USE] == (BYTE*)g_hl + g_each[k].use && VirtualProtect(&vt[VT_USE], 4, PAGE_READWRITE, &vold))
		{
			g_each[k].orig = (Use_t)vt[VT_USE];
			vt[VT_USE] = hooks[k];
			VirtualProtect(&vt[VT_USE], 4, vold, &vold);
		}
		else
			Log("aviso: vtable do %s nao confere - ele vai agir so em quem disparou", g_each[k].cls);
	}

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
		Log("slot %d: free %d flags 0x%x priv %p iuser1 %d health %.0f dead %d origin %.0f %.0f %.0f | modelo '%s' (%d) "
			"effects 0x%x render %d/%d/%d solid %d movetype %d", i, e->free, e->v.flags, e->pvPrivateData,
			e->v.iuser1, e->v.health, e->v.deadflag, e->v.origin.x, e->v.origin.y, e->v.origin.z,
			e->v.model ? STRING(e->v.model) : "", e->v.modelindex, e->v.effects, e->v.rendermode, (int)e->v.renderamt,
			e->v.renderfx, e->v.solid, e->v.movetype);
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
static std::string Describe(edict_t* e);

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
	if (!_stricmp(g_eng->pfnCmd_Argv(0), "cofmp_touch"))
	{
		Log("  %s absmin %.0f %.0f %.0f absmax %.0f %.0f %.0f nextthink %.1f", Describe(best).c_str(), best->v.absmin.x,
			best->v.absmin.y, best->v.absmin.z, best->v.absmax.x, best->v.absmax.y, best->v.absmax.z, best->v.nextthink);
		g_orig.pfnTouch(best, user);
		return;
	}
	g_orig.pfnUse(best, user);
}

static void Cmd_Teste(); // roteiro de teste, perto do TestScriptThink

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
	std::vector<int> held; // gatilhos que alguem tocou durante a espera
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

static void ClearHold()
{
	g_hold.active = false;
	g_hold.want = 0;
	g_hold.deadline = g_hold.releaseAt = g_hold.nextMsg = 0;
	memset(g_hold.frozen, 0, sizeof(g_hold.frozen));
	g_hold.held.clear();
}

static void StartHold(bool fresh)
{
	ClearHold();
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

static void Hook_Touch(edict_t* touched, edict_t* other);
static bool IsTriggerEnt(edict_t* e);

// Quem nasce dentro de um gatilho (o trigger_once da abertura do c_intro fica em volta do info_player_start) so o
// toca no primeiro frame, quando ainda estamos segurando; parado, o engine nao manda o toque de novo. Ao soltar,
// toca de novo os gatilhos segurados com quem ainda esta dentro deles.
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
	std::vector<int> held;
	held.swap(g_hold.held);
	for (int idx : held)
		for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
		{
			edict_t* trig = g_eng->pfnPEntityOfEntIndex(idx);
			edict_t* o = PlayerAt(k);
			if (IsNull(trig) || !IsTriggerEnt(trig) || trig->v.solid != SOLID_TRIGGER || !o || !IsAlive(o) || !Overlaps(o, trig))
				continue;
			Log("gatilho #%d '%s' tocado de novo por '%s' (estava dentro dele na espera)", idx, STRING(trig->v.classname),
				STRING(o->v.netname));
			Hook_Touch(trig, o);
		}
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

enum { GOD_WAIT = 1, GOD_CAM = 2, GOD_ADMIN = 4, GOD_SPEC = 8, GOD_SKIP = 16 }; // ADMIN: god mode do Home; SPEC: espectando;
                                                                              // SKIP: cena pulada sem camera
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
		Note("jogador %d imortal (%s)", i,
			why == GOD_WAIT ? "esperando o grupo" : why == GOD_CAM ? "cutscene" : why == GOD_ADMIN ? "god mode" : why == GOD_SPEC ? "espectando" : "cena pulada");
	}
	else if (before && !g_god[i].why)
	{
		// ligado antes da troca de mapa: o valor guardado ja era o nosso DAMAGE_NO
		p->v.takedamage = g_god[i].takedamage != DAMAGE_NO ? g_god[i].takedamage : DAMAGE_AIM;
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

// ---------------------------------------------------------------- comandos do Home (noclip, god, TP, espectar)
//
// A janela do Home de cada jogador manda "cmd cofmp_admin <noclip|god> <0|1>", "cmd cofmp_admin tp <n>" e
// "cmd cofmp_admin spec <n>" (0 = parar), "cmd cofmp_admin ammo <0|1>" e "cmd cofmp_admin stamina <0|1>". Noclip e god continuam ligados na troca de mapa (o engine
// desconecta e reconecta todos nela, entao so zeram quando um servidor novo abre).
// Espectar usa o modo "primeira pessoa" do espectador do HL, que o client.dll do CoF ainda tem: iuser1 = 4 e
// iuser2 = quem e seguido; o corpo de quem espia some, nao colide, nao toma dano e os monstros o ignoram.

static const int OBS_IN_EYE = 4;
static bool g_adminGod[33], g_noclip[33], g_infAmmo[33], g_infStamina[33];
static std::unordered_map<void*, int> g_ammoMax; // municao infinita: o maior pente visto em cada arma
// CBasePlayer, da tabela de save/restore do hl.dll (achado pelo Riique no PR #1): com m_bInfiniteStamina ligado o
// PreThink nao gasta estamina ao correr; m_fStamina (0..100) e o que vai para a barra no user message "Stamina"
static const int OFS_INF_STAMINA = 0x21e8; // bool m_bInfiniteStamina
static const int OFS_STAMINA = 0x21f0;     // float m_fStamina
static struct
{
	int target; // 0 = nao esta espectando
	Vector origin, angles;
	int movetype, solid;
} g_spec[33];
static bool g_specFix[33]; // espectava quando o mapa trocou: o corpo veio na transicao ainda invisivel

static edict_t* ClientAt(int i) // como PlayerAt, mas vale tambem para quem esta espectando
{
	edict_t* e = g_eng->pfnPEntityOfEntIndex(i);
	return !IsNull(e) && (e->v.flags & FL_CLIENT) && e->pvPrivateData ? e : NULL;
}

static void ShowBody(int i, edict_t* p)
{
	p->v.iuser1 = p->v.iuser2 = 0;
	p->v.effects &= ~EF_NODRAW;
	p->v.flags &= ~FL_NOTARGET;
	if (p->v.solid == SOLID_NOT)
		p->v.solid = SOLID_SLIDEBOX;
	p->v.movetype = g_noclip[i] ? MOVETYPE_NOCLIP : MOVETYPE_WALK;
	SetGod(i, GOD_SPEC, false);
	if (!g_god[i].why) // depois da troca de mapa o g_god ja foi zerado e o DAMAGE_NO veio no corpo
	{
		p->v.takedamage = DAMAGE_AIM;
		p->v.flags &= ~FL_GODMODE;
	}
}

static void StopSpec(int i, const char* why)
{
	if (i < 1 || i >= 33 || !g_spec[i].target)
		return;
	g_spec[i].target = 0;
	edict_t* p = ClientAt(i);
	if (!p)
		return;
	ShowBody(i, p);
	p->v.solid = g_spec[i].solid ? g_spec[i].solid : SOLID_SLIDEBOX;
	Vector pos = g_spec[i].origin;
	FindFreeSpot(pos, p);
	g_eng->pfnSetOrigin(p, pos);
	p->v.velocity = Vector(0, 0, 0);
	p->v.angles = p->v.v_angle = g_spec[i].angles;
	p->v.fixangle = 1;
	g_unstickAt[i] = gpGlobals->time + 0.2f;
	Log("jogador %d parou de espectar (%s)", i, why);
}

static void StartSpec(int i, edict_t* p, int t)
{
	edict_t* o = PlayerAt(t);
	if (t == i || !o || !IsAlive(o))
	{
		g_eng->pfnClientPrintf(p, print_center, "Esse jogador nao esta vivo no mapa");
		return;
	}
	if (g_spec[i].target)
	{
		g_spec[i].target = t; // so troca quem e seguido
		p->v.iuser2 = t;
		Log("jogador %d agora espectando o jogador %d", i, t);
		return;
	}
	if (!IsAlive(p))
	{
		g_eng->pfnClientPrintf(p, print_center, "Morto nao pode espectar");
		return;
	}
	SetGod(i, GOD_SPEC, true);
	g_spec[i].target = t;
	g_spec[i].origin = p->v.origin;
	g_spec[i].angles = p->v.v_angle;
	g_spec[i].solid = p->v.solid;
	p->v.iuser1 = OBS_IN_EYE;
	p->v.iuser2 = t;
	p->v.effects |= EF_NODRAW;
	p->v.solid = SOLID_NOT;
	p->v.movetype = MOVETYPE_NOCLIP;
	p->v.flags |= FL_NOTARGET;
	p->v.takedamage = DAMAGE_NO;
	Log("jogador %d espectando o jogador %d", i, t);
}

static void TeleportTo(int i, edict_t* p, int t)
{
	if (g_spec[i].target)
		StopSpec(i, "teleporte");
	edict_t* o = PlayerAt(t);
	if (t == i || !o || !IsAlive(o) || !IsAlive(p))
	{
		g_eng->pfnClientPrintf(p, print_center, "Esse jogador nao esta vivo no mapa");
		return;
	}
	Vector pos = o->v.origin;
	if (!FindFreeSpot(pos, p))
	{
		g_eng->pfnClientPrintf(p, print_center, "Sem espaco livre perto dele");
		return;
	}
	g_eng->pfnSetOrigin(p, pos);
	p->v.velocity = Vector(0, 0, 0);
	p->v.angles = p->v.v_angle = o->v.v_angle;
	p->v.fixangle = 1;
	Log("jogador %d teleportado ate o jogador %d (%.0f %.0f %.0f)", i, t, pos.x, pos.y, pos.z);
}

static void AdminCommand(int i, edict_t* p)
{
	const char* what = g_eng->pfnCmd_Argv(1);
	int n = atoi(g_eng->pfnCmd_Argv(2));
	if (!_stricmp(what, "noclip"))
	{
		g_noclip[i] = n != 0;
		if (!g_spec[i].target)
			p->v.movetype = g_noclip[i] ? MOVETYPE_NOCLIP : MOVETYPE_WALK;
		if (!g_noclip[i])
			g_unstickAt[i] = gpGlobals->time + 0.2f; // desligou dentro da parede
		g_eng->pfnClientPrintf(p, print_center, g_noclip[i] ? "Noclip ligado" : "Noclip desligado");
		Log("jogador %d: noclip %s", i, g_noclip[i] ? "ligado" : "desligado");
	}
	else if (!_stricmp(what, "god"))
	{
		g_adminGod[i] = n != 0;
		SetGod(i, GOD_ADMIN, g_adminGod[i]);
		g_eng->pfnClientPrintf(p, print_center, g_adminGod[i] ? "God mode ligado" : "God mode desligado");
		Log("jogador %d: god mode %s", i, g_adminGod[i] ? "ligado" : "desligado");
	}
	else if (!_stricmp(what, "ammo") || !_stricmp(what, "stamina"))
	{
		bool ammo = !_stricmp(what, "ammo");
		(ammo ? g_infAmmo : g_infStamina)[i] = n != 0;
		if (!ammo && !n) // desliga uma vez so: um mapa pode ligar a flag por conta propria
			Field<unsigned char>(p->pvPrivateData, OFS_INF_STAMINA) = 0;
		g_eng->pfnClientPrintf(p, print_center, ammo ? (n ? "Municao infinita ligada" : "Municao infinita desligada")
			: (n ? "Estamina infinita ligada" : "Estamina infinita desligada"));
		Log("jogador %d: %s infinita %s", i, ammo ? "municao" : "estamina", n ? "ligada" : "desligada");
	}
	else if (!_stricmp(what, "tp"))
		TeleportTo(i, p, n);
	else if (!_stricmp(what, "spec"))
	{
		if (n)
			StartSpec(i, p, n);
		else
			StopSpec(i, "pedido");
	}
}

static void AdminThink()
{
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* p = ClientAt(i);
		if (!p)
			continue;
		if (g_specFix[i])
		{
			g_specFix[i] = false;
			ShowBody(i, p);
			g_unstickAt[i] = gpGlobals->time + 0.2f;
			Log("jogador %d chegou no mapa novo sem espectar", i);
		}
		if (g_spec[i].target)
		{
			edict_t* o = PlayerAt(g_spec[i].target);
			if (!o || !IsAlive(o))
			{
				StopSpec(i, "o jogador seguido saiu ou morreu");
				continue;
			}
			// o corpo vai junto: o servidor manda o que esta perto dele, e a volta e para o lugar de antes
			g_eng->pfnSetOrigin(p, o->v.origin);
			p->v.velocity = o->v.velocity;
			p->v.iuser1 = OBS_IN_EYE;
			p->v.iuser2 = g_spec[i].target;
			p->v.takedamage = DAMAGE_NO;
			continue;
		}
		if (!IsPlayer(p) || !IsAlive(p))
			continue;
		if (g_noclip[i] && p->v.movetype != MOVETYPE_NOCLIP)
			p->v.movetype = MOVETYPE_NOCLIP;
		if (g_adminGod[i] && !(g_god[i].why & GOD_ADMIN))
			SetGod(i, GOD_ADMIN, true);
		void* pl = p->pvPrivateData;
		if (g_infStamina[i]) // reforca todo frame: renascer e trocar de mapa recriam o jogador
		{
			Field<unsigned char>(pl, OFS_INF_STAMINA) = 1;
			if (Field<float>(pl, OFS_STAMINA) < 100)
				Field<float>(pl, OFS_STAMINA) = 100;
		}
		void* it = Field<void*>(pl, OFS_ACTIVE_ITEM);
		if (g_infAmmo[i] && it && PevOf(it))
		{
			int& clip = Field<int>(it, OFS_ITEM_CLIP);
			auto f = g_ammoMax.find(it);
			if (clip < 0) // arma branca
				;
			else if (f == g_ammoMax.end() || clip > f->second)
				g_ammoMax[it] = clip;
			else if (clip < f->second)
				clip = f->second;
		}
	}
}

// ServerActivate: o mapa e novo; quem espectava chega pela transicao com o corpo escondido
static void ResetAdmin(bool newServer)
{
	for (int i = 0; i < 33; i++)
	{
		g_specFix[i] = g_spec[i].target != 0;
		g_spec[i].target = 0;
	}
	if (newServer)
	{
		memset(g_adminGod, 0, sizeof(g_adminGod));
		memset(g_noclip, 0, sizeof(g_noclip));
		memset(g_infAmmo, 0, sizeof(g_infAmmo));
		memset(g_infStamina, 0, sizeof(g_infStamina));
	}
	g_ammoMax.clear(); // as armas do mapa velho nao existem mais
}

// ---------------------------------------------------------------- pausa so com todos no menu
//
// O GameUI.dll do CoF manda "setpause" ao abrir o menu (Esc) e "unpause" ao fechar, sem olhar se o jogo e
// multiplayer: o Esc de qualquer um congelava todos. O cofmp.dll no jogo de cada um troca isso por
// "cmd cofmp_menu 1/0" (hud.cpp); aqui o servidor so pausa quando todos os jogadores estao com o menu aberto.
// O "setpause" vai pelo console do host, que o repassa ao servidor como se ele tivesse digitado.

static bool g_inMenu[33], g_menuPaused;

static void MenuCheck()
{
	int humans = 0, inMenu = 0;
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* p = ClientAt(i);
		if (!p || (p->v.flags & FL_FAKECLIENT))
			continue;
		humans++;
		inMenu += g_inMenu[i];
	}
	bool all = humans > 0 && inMenu == humans;
	if (all && !g_menuPaused)
	{
		g_menuPaused = true;
		g_eng->pfnServerCommand((char*)"setpause\n");
		Log("todos com o menu aberto (%d): jogo pausado", humans);
	}
	else if (!all && g_menuPaused)
	{
		g_menuPaused = false;
		g_eng->pfnServerCommand((char*)"unpause\n");
		Log("menu fechado por alguem (%d de %d ainda no menu): jogo despausado", inMenu, humans);
	}
}

static void MenuCommand(int i)
{
	bool open = atoi(g_eng->pfnCmd_Argv(1)) != 0;
	if (g_inMenu[i] == open)
		return;
	g_inMenu[i] = open;
	Note("jogador %d %s o menu", i, open ? "abriu" : "fechou");
	MenuCheck();
}

static void ResetMenu()
{
	memset(g_inMenu, 0, sizeof(g_inMenu));
	g_menuPaused = false; // servidor novo nasce despausado
}

// ---------------------------------------------------------------- save co-op
//
// O engine do CoF so salva pelo "savehack" (as maquinas de save: liga uma flag e chama o save) e recusa
// multiplayer ("Can't save multiplayer games."). O save em si (SV_SaveGameState) e o mesmo da troca de mapa,
// que ja funciona com varios jogadores; entao, no host, pulamos so o teste do maxplayers e ligamos a flag nos
// nossos saves. Os saves co-op ficam em SAVE\coop_<mapa>_<data>.sav, longe dos cofsave1..5 do single player.
// Qualquer jogador pode pedir (Home > Salvar, ou uma maquina de save: o hud.cpp troca o "savehack cofsaveN"
// por "cmd cofmp_save maquina"); o save e sempre do host. Carregar e pelo Home do host (load + tunel).

static const DWORD RVA_SAVE_MP_JE = 0x8233b;     // je depois de cmp [svs.maxclients], 1 no SaveGameCheck
static const DWORD RVA_SAVE_MP_MSG = 0x1be260;   // "Can't save multiplayer games.\n"
static const DWORD RVA_SAVEHACK_FLAG = 0x21b69;  // mov dword [flag], 1 no tratamento do "savehack"
static volatile int* g_saveFlag;

static void InstallSavePatch()
{
	BYTE* hw = (BYTE*)GetModuleHandleA("hw.dll");
	if (!hw)
		return;
	BYTE* je = hw + RVA_SAVE_MP_JE;
	BYTE* flag = hw + RVA_SAVEHACK_FLAG;
	if (je[-7] != 0x83 || je[-6] != 0x3d || je[-1] != 1 || (je[0] != 0x74 && je[0] != 0xeb) || je[2] != 0x68 ||
		*(DWORD*)(je + 3) != (DWORD)(hw + RVA_SAVE_MP_MSG) || memcmp(hw + RVA_SAVE_MP_MSG, "Can't save multiplayer", 22) ||
		flag[0] != 0xc7 || flag[1] != 0x05 || *(DWORD*)(flag + 6) != 1)
	{
		Log("aviso: o hw.dll e de outra versao - o save co-op fica desligado");
		return;
	}
	DWORD old;
	if (je[0] == 0x74 && VirtualProtect(je, 1, PAGE_EXECUTE_READWRITE, &old))
	{
		je[0] = 0xeb;
		VirtualProtect(je, 1, old, &old);
		FlushInstructionCache(GetCurrentProcess(), je, 1);
	}
	g_saveFlag = *(int**)(flag + 2);
}

static void CoopSave(const char* why, edict_t* asker)
{
	if (!g_saveFlag)
	{
		Log("save (%s) recusado: save co-op desligado (hw.dll de outra versao)", why);
		return;
	}
	edict_t* host = PlayerAt(1);
	if (!host || !IsAlive(host))
	{
		// o engine olha a vida do primeiro jogador (o host): "Can't savegame with a dead player"
		if (asker)
			g_eng->pfnClientPrintf(asker, print_center, "Nao da para salvar com o host morto");
		Log("save (%s) recusado: host morto", why);
		return;
	}
	SYSTEMTIME t;
	GetLocalTime(&t);
	char name[96], cmd[128];
	_snprintf(name, sizeof(name), "coop_%s_%02d%02d-%02d%02d%02d", STRING(gpGlobals->mapname), t.wDay, t.wMonth, t.wHour,
		t.wMinute, t.wSecond);
	name[sizeof(name) - 1] = 0;
	_snprintf(cmd, sizeof(cmd), "save %s\n", name);
	cmd[sizeof(cmd) - 1] = 0;
	*g_saveFlag = 1; // o "save" do console roda no proximo frame e zera a flag
	g_eng->pfnServerCommand(cmd);
	CenterPrintAll("Partida salva");
	Log("salvando a partida em SAVE\\%s.sav (%s)", name, why);
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
	ActorScope actor(IsPlayer(other) ? other : IsPlayer(touched) ? touched : NULL);
	if (cv_touchlog.value && IsTriggerEnt(touched) && !IsNull(other) && (other->v.flags & FL_CLIENT))
	{
		static float next[2048];
		int t = g_eng->pfnIndexOfEdict(touched);
		if (t < 2048 && gpGlobals->time >= next[t])
		{
			next[t] = gpGlobals->time + 2.0f;
			Note("toque: %s #%d com jogador #%d (hold %d, cutscene %d)", STRING(touched->v.classname), t,
				g_eng->pfnIndexOfEdict(other), (int)g_hold.active, (int)g_cutTrig.count(t));
		}
	}
	if (g_hold.active && ((IsTriggerEnt(touched) && IsPlayer(other)) || (IsTriggerEnt(other) && IsPlayer(touched))))
	{
		int t = g_eng->pfnIndexOfEdict(IsTriggerEnt(touched) ? touched : other);
		if (std::find(g_hold.held.begin(), g_hold.held.end(), t) == g_hold.held.end())
			g_hold.held.push_back(t);
		return;
	}
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
	g_eng->pfnCVarRegister(&cv_selfrespawn_time);
	g_eng->pfnCVarRegister(&cv_model);
	g_eng->pfnCVarRegister(&cv_start_wait);
	g_eng->pfnCVarRegister(&cv_start_timeout);
	g_eng->pfnCVarRegister(&cv_cutscene_wait);
	g_eng->pfnCVarRegister(&cv_touchlog);
	g_eng->pfnAddServerCommand((char*)"cofmp_exits", Cmd_Exits);
	g_eng->pfnAddServerCommand((char*)"cofmp_tp_exit", Cmd_TpExit);
	g_eng->pfnAddServerCommand((char*)"cofmp_debug", Cmd_Debug);
	g_eng->pfnAddServerCommand((char*)"cofmp_inv", Cmd_Inv);
	g_eng->pfnAddServerCommand((char*)"cofmp_give", Cmd_Give);
	g_eng->pfnAddServerCommand((char*)"cofmp_select", Cmd_Select);
	g_eng->pfnAddServerCommand((char*)"cofmp_bot", Cmd_Bot);
	g_eng->pfnAddServerCommand((char*)"cofmp_use", Cmd_Use);
	g_eng->pfnAddServerCommand((char*)"cofmp_touch", Cmd_Use); // como cofmp_use, mas toca (diagnostico)
	g_eng->pfnAddServerCommand((char*)"cofmp_tp", Cmd_Tp);
	g_eng->pfnAddServerCommand((char*)"cofmp_kill", Cmd_Kill);
	g_eng->pfnAddServerCommand((char*)"cofmp_revive", Cmd_Revive);
	g_eng->pfnAddServerCommand((char*)"cofmp_after", Cmd_After);
	g_eng->pfnAddServerCommand((char*)"cofmp_go", Cmd_Go);
	g_eng->pfnAddServerCommand((char*)"cofmp_map", Cmd_Map);
	g_eng->pfnAddServerCommand((char*)"cofmp_teste", Cmd_Teste);
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
	g_skipEndAt = -1000; // o relogio recomeca no mapa novo
	ResetTrace();
	g_netReset = true; // o carregamento do mapa nao conta como travada
	g_changing = false;
	g_scriptTeleportAt = -1000.0f;
	g_gather.active = false;
	memset(&g_arrive, 0, sizeof(g_arrive));
	memset(g_wasAlive, 0, sizeof(g_wasAlive));
	memset(g_unstickAt, 0, sizeof(g_unstickAt));
	ResetKeyring();
	ResetCutscene();
	ResetGod();
	g_objSet = false; // os string_t do mapa anterior nao valem mais
	g_objTarget = g_objMsg = 0;
	memset(&g_cut, 0, sizeof(g_cut));
	FindCutsceneTriggers();

	const char* map = STRING(gpGlobals->mapname);
	bool arrived = g_transit.pending && !_stricmp(map, g_transit.map.c_str());
	// mesmo mapa de novo sem changelevel nosso = todos morreram: volta ao checkpoint
	bool restart = !arrived && !_stricmp(map, g_lastMap.c_str());
	bool fromMenu = g_lastMap.empty() || !_strnicmp(g_lastMap.c_str(), "c_game_menu", 11);
	ResetAdmin(fromMenu);
	ResetMenu();
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

	// servidor recem-aberto (veio do menu) espera a lotacao; troca de mapa espera quem estava no mapa anterior. O
	// g_humansLast e a contagem do ultimo frame do mapa anterior: na troca de mapa o engine desconecta todo mundo
	// antes deste ServerActivate, e descontar essas saidas fazia o mapa seguinte esperar so 1 jogador.
	if (_strnicmp(map, "c_game_menu", 11))
		StartHold(fromMenu && !arrived && !restart);
	else
		ClearHold();

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
static void FloorWatchThink();
static void ViewFixThink();
static void FlashLog(edict_t* e);
static void ResetFloor();
static void TestScriptThink();
static void WatchBeat();
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
	WatchBeat();
	Hud_RunPending();
	g_orig.pfnStartFrame();
	AfterThink();
	NetStatsThink();
	if (!cv_enable.value || gpGlobals->maxClients <= 1)
		return;
	SkipThink();
	TestScriptThink();
	TraceThink();
	InvWatchThink();
	FloorWatchThink();
	ViewFixThink();
	BotThink();
	HoldThink();
	CutWaitThink();
	GodThink();
	AdminThink();
	CamJoinThink();
	GatherThink();
	ArriveThink();
	UnstickThink();
	RestoreThink();
	ShareThink();
	ReviveUseThink();
	RespawnThink();
	DeathPoseThink();
	ResendInventoryThink();
	SnapshotThink();
	ObjectiveThink();
	NotesThink();
}

// ---------------------------------------------------------------- modelo do jogador
//
// O co-op do CoF veste todos de policial (o cliente manda "model police1..4"). O modelo dos outros
// jogadores e escolhido pelo cliente a partir do "model" do userinfo, entao trocamos esse valor
// para models/player/simon/simon.mdl (gerado pelo instalador com a malha do Simon e as animacoes
// do policial). Quem nao tem o arquivo ve o modelo padrao, como antes.

static const char* WantedModel(edict_t* e, char* info)
{
	const char* want = cv_model.string;
	// bot nao tem canal de rede: a atualizacao do userinfo dele derruba o servidor na troca de mapa
	if (!cv_enable.value || !want || !*want || !info || gpGlobals->maxClients <= 1 || (e->v.flags & FL_FAKECLIENT))
		return nullptr;
	return want;
}

static void ForceModel(edict_t* e, char* info)
{
	const char* want = WantedModel(e, info);
	if (want && _stricmp(g_eng->pfnInfoKeyValue(info, (char*)"model"), want))
	{
		static int count[33];
		int i = g_eng->pfnIndexOfEdict(e);
		if (i > 0 && i < 33 && (++count[i] <= 3 || count[i] % 100 == 0))
			Log("modelo do jogador %d: '%s' -> '%s' (%d vez(es))", i, g_eng->pfnInfoKeyValue(info, (char*)"model"), want, count[i]);
		g_eng->pfnSetClientKeyValue(i, info, (char*)"model", (char*)want);
	}
}

// O ClientUserInfoChanged do jogo so faz uma coisa: troca o modelo de volta para policeN e manda
// MPModel. O engine chama ClientUserInfoChanged de novo a cada userinfo alterado, entao jogo e mod
// ficavam trocando o modelo todo frame: dezenas de svc_updateuserinfo confiaveis por segundo para
// cada cliente, e o canal confiavel do convidado atrasava minutos (inventario, lanterna...).
// Com o modelo ja trocado, o jogo nao e chamado.
static void Hook_ClientUserInfoChanged(edict_t* e, char* info)
{
	const char* want = WantedModel(e, info);
	if (want && !_stricmp(g_eng->pfnInfoKeyValue(info, (char*)"model"), want))
		return;
	g_orig.pfnClientUserInfoChanged(e, info);
	ForceModel(e, info);
}

static void Hook_ClientPutInServer(edict_t* e)
{
	g_orig.pfnClientPutInServer(e);
	// O engine guarda a visao de cada cliente como ponteiro e nao a zera na troca de mapa: quem saiu de um mapa
	// depois de uma camera chegava ao proximo olhando (para o engine) de um edict do mapa antigo. O PVS saia de
	// la e os outros jogadores nao eram mandados - ficavam invisiveis ate morrer e renascer (c_start2).
	if (cv_enable.value && gpGlobals->maxClients > 1)
		g_eng->pfnSetView(e, e);
	ForceModel(e, g_eng->pfnGetInfoKeyBuffer(e));
	Log("jogador %d '%s' entrou no mapa", g_eng->pfnIndexOfEdict(e), STRING(e->v.netname));
	WatchBeat();
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
	if (i > 0 && i < 33 && !strcmp(name, "cofmp_menu")) // Esc: menu do jogo aberto/fechado (hud.cpp)
	{
		MenuCommand(i);
		return;
	}
	if (i > 0 && i < 33 && !strcmp(name, "cofmp_admin")) // painel de comandos do Home
	{
		Log("jogador %d: cofmp_admin %s", i, g_eng->pfnCmd_Args());
		AdminCommand(i, e);
		return;
	}
	if (i > 0 && i < 33 && !strcmp(name, "cofmp_save")) // Home > Salvar, ou uma maquina de save
	{
		CoopSave(g_eng->pfnCmd_Argv(1), e);
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
	ActorScope actor(e);
	if (i > 0 && i < 33 && g_DualWield && e->pvPrivateData && cv_enable.value && gpGlobals->maxClients > 1 &&
		!strcmp(name, "inventorydualwield"))
	{
		g_DualWield(e->pvPrivateData, atoi(g_eng->pfnCmd_Argv(1)), atoi(g_eng->pfnCmd_Argv(2)));
		return;
	}
	if (!drop)
	{
		g_orig.pfnClientCommand(e);
		if (i > 0 && i < 33 && !strcmp(name, "flashlight"))
			FlashLog(e);
		return;
	}
	DropCommand(e);
}

// O jogo renasce o caido no pulo (perdendo as armas). Antes de cofmp_selfrespawn_time o pulo nao chega a ele.
static void Hook_PlayerPreThink(edict_t* e)
{
	int i = g_eng->pfnIndexOfEdict(e);
	if (cv_enable.value && gpGlobals->maxClients > 1 && i > 0 && i < 33 && g_deadSince[i] && !IsAlive(e) &&
		gpGlobals->time - g_deadSince[i] < cv_selfrespawn_time.value)
		e->v.button &= ~IN_JUMP;
	ActorScope actor(e);
	g_orig.pfnPlayerPreThink(e);
}

static void Hook_PlayerPostThink(edict_t* e)
{
	ActorScope actor(e);
	g_orig.pfnPlayerPostThink(e);
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
		g_ownView[i] = NULL;
		g_hold.frozen[i] = false;
		g_cut.waiting[i] = false;
		g_cut.frozen[i] = false;
		g_god[i] = {};
		// a troca de mapa tambem passa aqui (com o corpo ja no mapa novo): nada de voltar ao ponto antigo,
		// o corpo so reaparece onde esta (AdminThink)
		for (int k = 1; k < 33; k++)
			if (k == i || g_spec[k].target == i)
			{
				g_specFix[k] = g_specFix[k] || g_spec[k].target != 0;
				g_spec[k].target = 0;
			}
		g_inMenu[i] = false;
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
	out->pfnPlayerPreThink = Hook_PlayerPreThink;
	out->pfnPlayerPostThink = Hook_PlayerPostThink;
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
	// so a camera de cutscene e de todos; as outras visoes (cadeado, zoom, telescopio, a camera de fotos) sao so de
	// quem usa: o cadeado aparecia para os dois e o convidado ficava preso nele
	g_ownView[i] = NULL;
	if (on && _stricmp(STRING(view->v.classname), "trigger_camera"))
	{
		g_ownView[i] = (edict_t*)view;
		return;
	}
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
	memset(g_ownView, 0, sizeof(g_ownView));
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
// Depois da ultima camera o mapa ainda pode seguir sozinho (fade, falas, relays com delay) ate trocar de mapa: no
// c_intro foram ~35 s parados num limbo. Enter sem camera logo depois de um pulo acelera esse resto tambem, com
// todos imortais, ate o mapa trocar ou o tempo acabar.
static const float FREE_SKIP_AFTER = 90;  // s depois do fim do pulo em que Enter ainda vale
static const float FREE_SKIP_TIME = 60;   // s de jogo acelerados (com host_framerate 2 sao 30 frames)
static bool g_freeSkip;
static float g_freeSkipUntil;

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
	g_skipEndAt = gpGlobals->time;
	if (g_freeSkip)
	{
		g_freeSkip = false;
		for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
			SetGod(k, GOD_SKIP, false);
	}
	Log("cutscene: fim do pulo (%s)", why);
}

static void RequestSkip(int i)
{
	edict_t* p = PlayerAt(i);
	if (!p || g_changing || g_skipping || !cv_enable.value || gpGlobals->maxClients <= 1)
		return;
	if (!g_camOwner || IsNull(g_camView))
	{
		float since = gpGlobals->time - g_skipEndAt;
		if (since < 0 || since > FREE_SKIP_AFTER || !AllowFramerateInMP(true))
		{
			Note("jogador %d apertou Enter sem cutscene", i);
			return;
		}
		g_skipping = g_freeSkip = true;
		g_freeSkipUntil = gpGlobals->time + FREE_SKIP_TIME;
		for (int k = 1; k <= gpGlobals->maxClients && k < 33; k++)
		{
			edict_t* o = PlayerAt(k);
			if (!o || (o->v.flags & FL_FAKECLIENT))
				continue;
			SetGod(k, GOD_SKIP, true);
			g_eng->pfnClientPrintf(o, print_center, "Pulando o resto da cena...");
		}
		g_eng->pfnCVarSetFloat("host_framerate", SKIP_FRAMERATE);
		Log("cutscene: '%s' pulou o resto da cena sem camera (ate %.0f s de jogo)", STRING(p->v.netname), FREE_SKIP_TIME);
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
	if (g_skipping && g_freeSkip)
	{
		if (gpGlobals->time >= g_freeSkipUntil)
			StopSkip("tempo do pulo sem camera acabou");
	}
	else if (g_skipping && !g_camOwner)
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
	ResetFloor();
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

// m_pfnThink/Touch/Use (+0x78/+0x7c/+0x80) como endereco do hl.dll sem relocacao, para achar no disassembly
static unsigned HlAddr(edict_t* e, int ofs)
{
	unsigned p = e->pvPrivateData ? Field<unsigned>(e->pvPrivateData, ofs) : 0;
	return p && g_hl ? p - (unsigned)(size_t)g_hl + 0x10000000 : p;
}

// estado de uma arma/item no log: onde esta, se aparece e o que o hl.dll olha no drop e no pegar
static std::string Describe(edict_t* e)
{
	if (!e)
		return "(nenhum)";
	char buf[600];
	entvars_t* v = &e->v;
	// +0x110 (arquivo do inventario) so existe nas armas; nos outros itens o campo e outra coisa
	bool weapon = e->pvPrivateData && !strncmp(STRING(v->classname), "weapon_", 7);
	int file = weapon ? Field<int>(e->pvPrivateData, OFS_ITEM_FILE) : 0;
	void* holder = weapon ? Field<void*>(e->pvPrivateData, OFS_ITEM_PLAYER) : NULL;
	_snprintf(buf, sizeof(buf) - 1,
		"#%d '%s' livre %d modelo '%s' (%d) arquivo '%s' effects %d solid %d movetype %d iuser3 %d iuser4 %d "
		"target '%s' spawnflags 0x%x flags 0x%x armor %.0f dono #%d aiment #%d m_pPlayer #%d em %.0f %.0f %.0f "
		"think %x touch %x use %x",
		g_eng->pfnIndexOfEdict(e), STRING(v->classname), e->free, v->model ? STRING(v->model) : "", v->modelindex,
		file ? STRING(file) : "", v->effects, v->solid, v->movetype, v->iuser3, v->iuser4,
		v->target ? STRING(v->target) : "", v->spawnflags, v->flags, v->armorvalue,
		v->owner ? g_eng->pfnIndexOfEdict(v->owner) : 0, v->aiment ? g_eng->pfnIndexOfEdict(v->aiment) : 0,
		holder && PevOf(holder) ? g_eng->pfnIndexOfEdict(PevOf(holder)->pContainingEntity) : 0, v->origin.x,
		v->origin.y, v->origin.z, HlAddr(e, 0x78), HlAddr(e, 0x7c), HlAddr(e, 0x80));
	buf[sizeof(buf) - 1] = 0;
	return buf;
}

// A arma do slot (pelo arquivo do inventario; o nome de classe guardado no slot nao bate com o da arma).
static void* SlotWeapon(void* pl, int slot)
{
	int file = Field<int>(pl, OFS_INV_SLOT + slot * 4);
	if (!file || !*STRING(file))
		return NULL;
	const char* f = STRING(file);
	std::string name = ItemName(f);
	void* found = NULL;
	ForEachItem(pl, [&](void* it) {
		entvars_t* pev = PevOf(it);
		if (found || !pev)
			return;
		int wf = Field<int>(it, OFS_ITEM_FILE);
		if ((wf && !_stricmp(STRING(wf), f)) || !_stricmp(STRING(pev->classname), name.c_str()))
			found = it;
	});
	return found;
}

static bool HasItem(void* pl, void* item)
{
	bool in = false;
	ForEachItem(pl, [&](void* it) { in |= it == item; });
	return in;
}

static bool OnFloor(edict_t* w)
{
	return !(w->v.effects & EF_NODRAW) && w->v.modelindex && w->v.solid != 0 && !w->v.aiment;
}

// O que o drop do proprio hl.dll faz no fim (CBasePlayer::DropPlayerItem): arma na posicao do jogador,
// Spawn de novo (modelo do chao e FallInit, que faz cair e deixa pegar com o Usar) e DROP_TO_FLOOR.
static void PutOnFloor(edict_t* w, edict_t* player)
{
	Field<void*>(w->pvPrivateData, OFS_ITEM_PLAYER) = NULL;
	w->v.origin = player->v.origin;
	w->v.movetype = 0;
	w->v.solid = 1;
	w->v.aiment = NULL;
	w->v.owner = NULL;
	w->v.effects &= ~EF_NODRAW;
	g_orig.pfnSpawn(w);
	g_eng->pfnDropToFloor(w);
	w->v.armorvalue = 666.0f; // marca de item largado, como o jogo poe
}

// inventorydrop N: a arma do slot N antes e depois, e o destino dela nos 10 s seguintes. Se o hl.dll tirou
// a arma do jogador mas nao a deixou no chao (some no MP), o mod poe no chao.
static void DropCommand(edict_t* e)
{
	int i = g_eng->pfnIndexOfEdict(e), slot = atoi(g_eng->pfnCmd_Argv(1));
	void* pl = e->pvPrivateData;
	void* it = NULL;
	edict_t* w = NULL;
	std::string cls;
	if (pl && slot >= 0 && slot < 7)
	{
		it = SlotWeapon(pl, slot);
		if (it && PevOf(it))
			w = PevOf(it)->pContainingEntity;
		if (w)
			cls = STRING(w->v.classname);
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
		Log("  depois do drop: a arma foi liberada; criando outra '%s' no chao", cls.c_str());
		w = g_eng->pfnCreateNamedEntity(EngString(cls));
		if (IsNull(w))
			return;
		w->v.origin = e->v.origin;
		g_orig.pfnSpawn(w);
		g_eng->pfnDropToFloor(w);
		serial = w->serialnumber;
	}
	else if (!HasItem(pl, it) && !OnFloor(w))
	{
		Log("  depois do drop: %s", Describe(w).c_str());
		PutOnFloor(w, e);
		Log("  o jogo nao deixou a arma no chao; o mod pos: %s", Describe(w).c_str());
	}
	else
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

// Itens no chao perto de cada jogador: quando um sai do chao (pegou com o Usar, sumiu, foi apagado), uma linha
// com quem estava perto e os slots dele antes e depois. O pegar do CoF e pelo Usar, que nao passa pelo Touch.
struct FloorItem
{
	edict_t* e;
	int serial;
	std::string cls;
	int who;
	std::string slots;
};
static std::vector<FloorItem> g_floor;

static bool IsLoot(edict_t* e)
{
	const char* c = STRING(e->v.classname);
	return !strncmp(c, "weapon_", 7) || !strncmp(c, "item_", 5) || !strncmp(c, "ammo_", 5);
}

static void FloorWatchThink()
{
	static float next;
	if (gpGlobals->time < next && gpGlobals->time > next - 1.0f)
		return;
	next = gpGlobals->time + 0.2f;
	for (size_t k = 0; k < g_floor.size();)
	{
		FloorItem& f = g_floor[k];
		edict_t* p = PlayerAt(f.who);
		bool gone = f.e->free || f.e->serialnumber != f.serial;
		if (gone || !OnFloor(f.e) || f.e->v.owner)
		{
			std::string after = p ? SlotsOf(p->pvPrivateData) : "?";
			Log("item '%s' saiu do chao perto do jogador %d: %s; slots [%s] -> [%s]", f.cls.c_str(), f.who,
				gone ? "liberado" : Describe(f.e).c_str(), f.slots.c_str(), after.c_str());
			g_floor.erase(g_floor.begin() + k);
			continue;
		}
		Vector d = p ? f.e->v.origin - p->v.origin : Vector(9999, 0, 0);
		if (d.Length() > 200)
		{
			g_floor.erase(g_floor.begin() + k);
			continue;
		}
		f.slots = SlotsOf(p->pvPrivateData);
		k++;
	}
	for (int i = 1; i <= gpGlobals->maxClients && i < 33 && g_floor.size() < 64; i++)
	{
		edict_t* p = PlayerAt(i);
		if (!p || !IsAlive(p) || (p->v.flags & FL_FAKECLIENT))
			continue;
		for (edict_t* e = g_eng->pfnFindEntityInSphere(NULL, p->v.origin, 120); !IsNull(e) && g_floor.size() < 64;
			 e = g_eng->pfnFindEntityInSphere(e, p->v.origin, 120))
		{
			if (!e->pvPrivateData || !IsLoot(e) || !OnFloor(e) || e->v.owner)
				continue;
			bool known = false;
			for (const FloorItem& f : g_floor)
				known |= f.e == e && f.serial == e->serialnumber;
			if (!known)
				g_floor.push_back({ e, e->serialnumber, STRING(e->v.classname), i, SlotsOf(p->pvPrivateData) });
		}
	}
}

// Roteiro de teste: arquivo de linhas "segundos [jN] comando". Cada comando vai ao console do jogador N (padrao 1),
// como se ele digitasse; no jogador 2 (o amigo) roda no PC dele. A contagem comeca quando todos os jogadores do
// roteiro estao vivos. Carrega ao abrir o jogo (da variavel de ambiente COFMP_TESTE ou, se existir,
// Mod coop\teste.txt) ou com "cofmp_teste [arquivo]" no console do host (padrao: Mod coop\teste.txt).
// Comandos do mod: !olhar P (angulo vertical, positivo = baixo), !criar classe (item novo 48 unidades a frente),
// !usar (usa o item mais perto com o botao Usar apertado), !aviso texto (no meio da tela do jogador).
struct TestStep
{
	float at;
	int who;
	std::string cmd;
};
static std::vector<TestStep> g_test;
static float g_testStart;
static std::string g_modDir;

static void LoadTestScript(const char* path, bool quiet = false)
{
	g_test.clear();
	g_testStart = 0;
	FILE* f = fopen(path, "r");
	if (!f)
	{
		if (!quiet)
			Log("teste: nao abriu %s", path);
		return;
	}
	char line[256];
	while (fgets(line, sizeof(line), f))
	{
		char* cmd = NULL;
		float at = (float)strtod(line, &cmd);
		int who = 1;
		while (*cmd == ' ' || *cmd == '\t')
			cmd++;
		if ((*cmd == 'j' || *cmd == 'J') && cmd[1] >= '1' && cmd[1] <= '9' && cmd[2] == ' ')
		{
			who = cmd[1] - '0';
			for (cmd += 3; *cmd == ' ' || *cmd == '\t'; cmd++)
				;
		}
		size_t n = strcspn(cmd, "\r\n");
		if (n && *line != '/')
			g_test.push_back({ at, who, std::string(cmd, n) });
	}
	fclose(f);
	Log("teste: %d comando(s) de %s", (int)g_test.size(), path);
}

static void Cmd_Teste()
{
	std::string path = g_eng->pfnCmd_Argc() > 1 ? g_eng->pfnCmd_Argv(1) : g_modDir + "\\teste.txt";
	LoadTestScript(path.c_str());
}

static void TestStepRun(const TestStep& s, edict_t* p)
{
	const std::string& cmd = s.cmd;
	Log("teste: %.1f s: jogador %d: %s", s.at, s.who, cmd.c_str());
	if (!strncmp(cmd.c_str(), "!olhar ", 7))
	{
		p->v.angles.x = p->v.v_angle.x = (float)atof(cmd.c_str() + 7);
		p->v.fixangle = 1;
	}
	else if (!strncmp(cmd.c_str(), "!criar ", 7))
	{
		edict_t* w = g_eng->pfnCreateNamedEntity(EngString(cmd.c_str() + 7));
		if (IsNull(w))
			return;
		float yaw = p->v.v_angle.y * 3.14159265f / 180.0f;
		w->v.origin = p->v.origin + Vector(cosf(yaw) * 48, sinf(yaw) * 48, 0);
		g_orig.pfnSpawn(w);
		g_eng->pfnDropToFloor(w);
		Log("teste: criada %s", Describe(w).c_str());
	}
	else if (!strncmp(cmd.c_str(), "!aviso ", 7))
		g_eng->pfnClientPrintf(p, print_center, (cmd.substr(7) + "\n").c_str());
	else if (cmd == "!usar")
	{
		edict_t* best = NULL;
		for (edict_t* e = g_eng->pfnFindEntityInSphere(NULL, p->v.origin, 96); !IsNull(e);
			 e = g_eng->pfnFindEntityInSphere(e, p->v.origin, 96))
			if (e->pvPrivateData && IsLoot(e) && OnFloor(e) && !e->v.owner)
				best = e;
		if (!best)
		{
			Log("teste: nenhum item no chao perto do jogador %d", s.who);
			return;
		}
		std::string before = SlotsOf(p->pvPrivateData), desc = Describe(best);
		int serial = best->serialnumber;
		p->v.button |= IN_USE; // o hl.dll so deixa pegar com o Usar apertado
		g_orig.pfnUse(best, p);
		p->v.button &= ~IN_USE;
		Log("teste: usou %s; slots [%s] -> [%s]; item depois: %s", desc.c_str(), before.c_str(),
			SlotsOf(p->pvPrivateData).c_str(),
			best->free || best->serialnumber != serial ? "liberado" : Describe(best).c_str());
	}
	else
		g_eng->pfnClientCommand(p, (char*)"%s\n", cmd.c_str());
}

static void TestScriptThink()
{
	static bool fromEnv;
	if (!fromEnv)
	{
		fromEnv = true;
		if (const char* path = getenv("COFMP_TESTE"))
			LoadTestScript(path);
		else
			LoadTestScript((g_modDir + "\\teste.txt").c_str(), true);
	}
	if (g_test.empty())
		return;
	if (!g_testStart)
	{
		for (const TestStep& s : g_test)
		{
			edict_t* p = PlayerAt(s.who);
			if (!p || !IsAlive(p))
				return;
		}
		g_testStart = gpGlobals->time;
		Log("teste: comecou");
	}
	while (!g_test.empty() && gpGlobals->time - g_testStart >= g_test.front().at)
	{
		TestStep s = g_test.front();
		g_test.erase(g_test.begin());
		edict_t* p = PlayerAt(s.who);
		if (p)
			TestStepRun(s, p);
		else
			Log("teste: %.1f s: jogador %d nao esta no jogo; pulando '%s'", s.at, s.who, s.cmd.c_str());
	}
}

static void ResetFloor()
{
	g_floor.clear();
}

// A lanterna da propria tela so e desenhada se o cliente acha que olha pelos proprios olhos: a mensagem
// ViewEntity (m_iViewEntity do jogador) tem que ser 0 ou o indice dele. Fora de uma cutscene, se ficou com
// outra entidade (outro jogador, uma camera que ja acabou), devolve para o proprio jogador.
static void ViewFixThink()
{
	if (g_camOwner)
		return;
	for (int i = 1; i <= gpGlobals->maxClients && i < 33; i++)
	{
		edict_t* p = PlayerAt(i);
		if (!p || (p->v.flags & FL_FAKECLIENT) || (g_ownView[i] && !g_ownView[i]->free))
			continue;
		int& view = Field<int>(p->pvPrivateData, OFS_VIEW_ENTITY);
		if (!view || view == i)
			continue;
		edict_t* v = g_eng->pfnPEntityOfEntIndex(view);
		Log("jogador %d: visao presa na entidade #%d ('%s'), o que esconde a lanterna dele; devolvendo", i, view,
			v && !v->free ? STRING(v->v.classname) : "");
		view = i; // o hl.dll reenvia o ViewEntity no proximo UpdateClientData
	}
}

static void FlashLog(edict_t* e)
{
	int i = g_eng->pfnIndexOfEdict(e);
	if (!e->pvPrivateData)
		return;
	Log("jogador %d lanterna: flags %d, visao #%d, bateria %d, effects %d", i,
		Field<int>(e->pvPrivateData, OFS_FLASH_FLAGS), Field<int>(e->pvPrivateData, OFS_VIEW_ENTITY),
		Field<int>(e->pvPrivateData, OFS_FLASH_BATTERY), e->v.effects);
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

// ---------------------------------------------------------------- vigia de travamento
//
// Se o servidor passa 8 s sem rodar um frame (jogo travado, ou so carregando), anota no log onde a thread
// principal esta: EIP e os enderecos de retorno achados na pilha, como modulo+deslocamento. Repete com 16, 32,
// 64 e 128 s parado. Com a thread suspensa so copia a pilha (nada que pegue lock); os nomes dos modulos vem do
// K32EnumProcessModules, que le a lista do PEB sem o loader lock. Aos 30, 90 e 180 s de jogo anota tambem quantos
// frames o servidor rodou e as janelas do jogo (para o caso de tela parada com o jogo rodando por tras).

static HANDLE g_mainThread;
static volatile LONG g_watchBeat;

// COFMP_PRINT=1 (posto pelos .bat de diagnostico): o vigia tira um print do jogo a cada etapa e desliga mais um
// grupo de efeitos de tela, para achar qual deixa a tela toda preta (placas AMD). Os comandos tem que rodar na
// thread do jogo, entao vao aqui. g_watchShot = numero da etapa + 1.
static volatile LONG g_watchShot;

static const char* const g_watchCvars[] = { "gl_renderer", "gl_posteffects", "gl_grayscale", "gl_screenblur",
	"gl_glows", "gl_noshaders", "gl_bump", "gl_specular", "gl_highspecular", "gl_dynlight", "gl_blurtest",
	"cl_vignette", "cl_noiseeffect", "gl_brightness", "gl_contrast", "gl_gamma", "gl_opacity" };
static const char* const g_watchSteps[] = {
	"gl_posteffects 0; gl_grayscale 0",
	"gl_screenblur 0; gl_glows 0; cl_vignette 0; cl_noiseeffect 0",
	"gl_noshaders 1; gl_bump 0; gl_specular 0; gl_highspecular 0; gl_dynlight 0",
	"gl_renderer 0",
};
static const int WATCH_STEPS = sizeof(g_watchSteps) / sizeof(g_watchSteps[0]) + 1;
// valores dos cvars antes das etapas, devolvidos no fim (varios sao salvos no config.cfg ao sair)
static char g_watchRestore[400];

// Teste do OpenGL, chamado na thread do jogo (o contexto do OpenGL esta ativo nela): anota o formato de pixel e o
// estado que o jogo deixou; com red, pinta a tela de vermelho direto (glClear + SwapBuffers). Junto com a foto da
// janela como o Windows mostra (WatchSaveWindow), separa "o jogo desenha preto" de "o desenho nao chega na tela".
// As funcoes vem do opengl32.dll que o jogo carregou (o do Paranoia, na pasta do jogo), por GetProcAddress.
#define GL_FN(ret, name, args) ret(WINAPI* name) args = (ret(WINAPI*) args)GetProcAddress(gl, #name)
static void WatchSaveWindow(HWND w, const char* file);
static void WatchHooks();
struct WatchMod { DWORD base, size; char name[48]; };
static int WatchModules(WatchMod* out, int max);
static int WatchName(char* buf, int size, const WatchMod* m, int n, DWORD a);

static void WatchGL(bool red)
{
	char buf[1000];
	HMODULE gl = GetModuleHandleA("opengl32.dll");
	if (!gl)
	{
		LogFile_Write("trava", "opengl: opengl32.dll nao carregado");
		return;
	}
	char glPath[MAX_PATH] = "";
	GetModuleFileNameA(gl, glPath, sizeof(glPath));
	GL_FN(HGLRC, wglGetCurrentContext, ());
	GL_FN(HDC, wglGetCurrentDC, ());
	GL_FN(BOOL, wglSwapBuffers, (HDC));
	GL_FN(const char*, glGetString, (unsigned));
	GL_FN(void, glGetIntegerv, (unsigned, int*));
	GL_FN(void, glGetFloatv, (unsigned, float*));
	GL_FN(unsigned char, glIsEnabled, (unsigned));
	GL_FN(unsigned, glGetError, ());
	GL_FN(void, glReadBuffer, (unsigned));
	GL_FN(void, glReadPixels, (int, int, int, int, unsigned, unsigned, void*));
	GL_FN(void, glDisable, (unsigned));
	GL_FN(void, glEnable, (unsigned));
	GL_FN(void, glColorMask, (unsigned char, unsigned char, unsigned char, unsigned char));
	GL_FN(void, glClearColor, (float, float, float, float));
	GL_FN(void, glClear, (unsigned));
	GL_FN(void, glFinish, ());
	if (!wglGetCurrentContext || !wglGetCurrentDC || !wglSwapBuffers || !glGetString || !glGetIntegerv ||
		!glGetFloatv || !glIsEnabled || !glGetError || !glReadBuffer || !glReadPixels || !glDisable || !glEnable ||
		!glColorMask || !glClearColor || !glClear || !glFinish)
	{
		snprintf(buf, sizeof(buf), "opengl: faltam funcoes em %s", glPath);
		LogFile_Write("trava", buf);
		return;
	}
	HGLRC rc = wglGetCurrentContext();
	HDC dc = wglGetCurrentDC();
	HWND w = dc ? WindowFromDC(dc) : NULL;
	if (!rc || !dc)
	{
		snprintf(buf, sizeof(buf), "opengl: sem contexto ativo nesta thread (contexto %p, dc %p, %s)", rc, dc, glPath);
		LogFile_Write("trava", buf);
		return;
	}
	unsigned err0 = glGetError();
	while (glGetError()) {}
	int vp[4] = {}, drawBuf = 0, readBuf = 0, fbo = -1, mask[4] = {}, bits[6] = {};
	float color[4] = {}, clear[4] = {};
	glGetIntegerv(0x0BA2, vp);   // GL_VIEWPORT
	glGetIntegerv(0x0C01, &drawBuf);  // GL_DRAW_BUFFER
	glGetIntegerv(0x0C02, &readBuf);  // GL_READ_BUFFER
	glGetIntegerv(0x0C23, mask);      // GL_COLOR_WRITEMASK
	glGetIntegerv(0x8CA6, &fbo);      // GL_FRAMEBUFFER_BINDING (erro se nao tiver FBO)
	for (int i = 0; i < 6; i++)       // GL_RED/GREEN/BLUE/ALPHA/DEPTH/STENCIL_BITS
		glGetIntegerv(i < 4 ? 0x0D52 + i : i == 4 ? 0x0D56 : 0x0D57, &bits[i]);
	glGetFloatv(0x0B00, color);       // GL_CURRENT_COLOR
	glGetFloatv(0x0C22, clear);       // GL_COLOR_CLEAR_VALUE
	bool scissor = glIsEnabled(0x0C11) != 0;
	int n = snprintf(buf, sizeof(buf), "opengl (%s): %s | %s | %s | janela %p", glPath, glGetString(0x1F00),
		glGetString(0x1F01), glGetString(0x1F02), w);
	PIXELFORMATDESCRIPTOR pfd = {};
	int pf = GetPixelFormat(dc);
	if (pf && DescribePixelFormat(dc, pf, sizeof(pfd), &pfd))
		n += snprintf(buf + n, sizeof(buf) - n, " | pixel format %d: flags %#lx (%s%s%s) cor %d alfa %d prof %d stencil %d",
			pf, pfd.dwFlags, pfd.dwFlags & PFD_DOUBLEBUFFER ? "doublebuffer " : "",
			pfd.dwFlags & PFD_GENERIC_FORMAT ? "GENERICO " : "", pfd.dwFlags & PFD_GENERIC_ACCELERATED ? "gen-acel " : "",
			pfd.cColorBits, pfd.cAlphaBits, pfd.cDepthBits, pfd.cStencilBits);
	else
		n += snprintf(buf + n, sizeof(buf) - n, " | pixel format %d (erro %lu)", pf, GetLastError());
	if (n > 0 && n < (int)sizeof(buf))
		n += snprintf(buf + n, sizeof(buf) - n, " | bits r%d g%d b%d a%d prof%d sten%d | viewport %d,%d %dx%d | draw %#x read %#x"
			" fbo %d | colormask %d%d%d%d | scissor %d fragprog %d blend %d depth %d alphatest %d tex2d %d"
			" | cor %.2f,%.2f,%.2f,%.2f clear %.2f,%.2f,%.2f,%.2f | erro antes %#x, ao ler %#x",
			bits[0], bits[1], bits[2], bits[3], bits[4], bits[5], vp[0], vp[1], vp[2], vp[3], drawBuf, readBuf, fbo,
			mask[0], mask[1], mask[2], mask[3], scissor, glIsEnabled(0x8804), glIsEnabled(0x0BE2), glIsEnabled(0x0B71),
			glIsEnabled(0x0BC0), glIsEnabled(0x0DE1), color[0], color[1], color[2], color[3], clear[0], clear[1],
			clear[2], clear[3], err0, glGetError());
	LogFile_Write("trava", buf);

	// pixel do meio da tela no buffer de tras (GL_BACK) ou da frente (GL_FRONT)
	int cx = vp[0] + vp[2] / 2, cy = vp[1] + vp[3] / 2;
	auto pixel = [&](unsigned which, unsigned char* px) {
		glReadBuffer(which);
		glReadPixels(cx, cy, 1, 1, 0x1908, 0x1401, px);  // GL_RGBA, GL_UNSIGNED_BYTE
	};
	HWND root = w ? GetAncestor(w, GA_ROOT) : NULL;
	if (!red)
	{
		unsigned char front[4] = {}, back[4] = {};
		pixel(0x0405, back);
		pixel(0x0404, front);
		glReadBuffer(readBuf);
		snprintf(buf, sizeof(buf), "opengl como o jogo deixou: pixel do meio (%d,%d) atras %d,%d,%d,%d, na frente %d,%d,%d,%d, erro %#x",
			cx, cy, back[0], back[1], back[2], back[3], front[0], front[1], front[2], front[3], glGetError());
		LogFile_Write("trava", buf);
		if (root)
			WatchSaveWindow(root, "tela_normal.bmp");
		WatchHooks();
		return;
	}

	// quatro jeitos de por uma cor na janela, cada um com uma foto da tela: troca de buffers pelo opengl32 que o
	// jogo usa, pelo gdi32, desenho direto no buffer da frente e pintura do Windows (GDI), sem OpenGL
	GL_FN(void, glDrawBuffer, (unsigned));
	GL_FN(void, glFlush, ());
	HMODULE gdi = GetModuleHandleA("gdi32.dll");
	BOOL(WINAPI* gdiSwap)(HDC) = gdi ? (BOOL(WINAPI*)(HDC))GetProcAddress(gdi, "SwapBuffers") : NULL;
	static const char* const names[] = { "1_vermelho_wglSwapBuffers", "2_verde_SwapBuffers_gdi32",
		"3_azul_desenho_na_frente", "4_amarelo_gdi" };
	static const float cols[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	glDisable(0x0C11);
	glColorMask(1, 1, 1, 1);
	for (int t = 0; t < 4; t++)
	{
		unsigned char front[4] = {}, back[4] = {};
		BOOL ok = TRUE;
		DWORD le = 0;
		if (t < 3)
		{
			if (t == 2 && glDrawBuffer)
				glDrawBuffer(0x0404);
			glClearColor(cols[t][0], cols[t][1], cols[t][2], 1);
			glClear(0x4000);  // GL_COLOR_BUFFER_BIT
			glFinish();
			pixel(0x0405, back);
			SetLastError(0);
			if (t == 0)
				ok = wglSwapBuffers(dc);
			else if (t == 1)
				ok = gdiSwap && gdiSwap(dc);
			else
			{
				if (glFlush)
					glFlush();
				if (glDrawBuffer)
					glDrawBuffer(drawBuf);
			}
			le = GetLastError();
			glFinish();
		}
		else if (w)
		{
			HDC wdc = GetDC(w);
			RECT r;
			GetClientRect(w, &r);
			HBRUSH b = CreateSolidBrush(RGB(255, 255, 0));
			ok = FillRect(wdc, &r, b);
			DeleteObject(b);
			GdiFlush();
			ReleaseDC(w, wdc);
		}
		Sleep(500);
		pixel(0x0404, front);
		glReadBuffer(readBuf);
		snprintf(buf, sizeof(buf), "teste %s: retorno %d (erro %lu), pixel do meio atras %d,%d,%d, na frente %d,%d,%d, erro gl %#x",
			names[t], ok, le, back[0], back[1], back[2], front[0], front[1], front[2], glGetError());
		LogFile_Write("trava", buf);
		if (root)
		{
			char file[64];
			snprintf(file, sizeof(file), "tela_%s.bmp", names[t]);
			WatchSaveWindow(root, file);
		}
	}
	glClearColor(clear[0], clear[1], clear[2], clear[3]);
	glColorMask(mask[0], mask[1], mask[2], mask[3]);
	if (scissor)
		glEnable(0x0C11);
}

// Modulos carregados no jogo e o comeco das funcoes de troca de buffer: um jmp logo no comeco quer dizer que
// algum programa (overlay da Steam/AMD/Discord, gravador de tela...) se pendurou nelas.
static void WatchHooks()
{
	static WatchMod mods[400];
	int n = WatchModules(mods, 400);
	std::string line = "modulos:";
	for (int i = 0; i < n; i++)
	{
		if (line.size() > 900)
		{
			LogFile_Write("trava", line.c_str());
			line = "modulos (cont.):";
		}
		line += ' ';
		line += mods[i].name;
	}
	LogFile_Write("trava", line.c_str());
	for (int i = 0; i < n; i++)
	{
		const char* fn = !_stricmp(mods[i].name, "opengl32.dll") ? "wglSwapBuffers" :
			!_stricmp(mods[i].name, "gdi32.dll") || !_stricmp(mods[i].name, "gdi32full.dll") ? "SwapBuffers" : NULL;
		if (!fn)
			continue;
		HMODULE h = (HMODULE)(size_t)mods[i].base;
		const BYTE* p = (const BYTE*)GetProcAddress(h, fn);
		char path[MAX_PATH] = "", buf[700];
		GetModuleFileNameA(h, path, sizeof(path));
		if (!p)
		{
			snprintf(buf, sizeof(buf), "%s em %s: nao exporta", fn, path);
			LogFile_Write("trava", buf);
			continue;
		}
		int k = snprintf(buf, sizeof(buf), "%s em %s:", fn, path);
		for (int j = 0; j < 8; j++)
			k += snprintf(buf + k, sizeof(buf) - k, " %02x", p[j]);
		DWORD to = 0;
		if (p[0] == 0xE9)
			to = (DWORD)(size_t)p + 5 + *(const LONG*)(p + 1);
		else if (p[0] == 0xFF && p[1] == 0x25)
			to = **(const DWORD* const*)(p + 2);
		if (to)
		{
			k += snprintf(buf + k, sizeof(buf) - k, " -> pula para");
			WatchName(buf + k, sizeof(buf) - k, mods, n, to);
		}
		LogFile_Write("trava", buf);
	}
}

// foto da area da janela do jogo como aparece na tela (o que o Windows mostra), em Mod coop\<file>
static void WatchSaveWindow(HWND w, const char* file)
{
	RECT r;
	POINT p = {};
	if (!GetClientRect(w, &r) || !ClientToScreen(w, &p) || r.right <= 0 || r.bottom <= 0)
		return;
	int cx = r.right, cy = r.bottom, stride = (cx * 3 + 3) & ~3;
	HDC scr = GetDC(NULL);
	HDC mem = CreateCompatibleDC(scr);
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = cx;
	bi.bmiHeader.biHeight = cy;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 24;
	bi.bmiHeader.biCompression = BI_RGB;
	void* pix = NULL;
	HBITMAP bm = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &pix, NULL, 0);
	char buf[MAX_PATH + 200];
	if (bm && pix)
	{
		HGDIOBJ old = SelectObject(mem, bm);
		BitBlt(mem, 0, 0, cx, cy, scr, p.x, p.y, SRCCOPY | CAPTUREBLT);
		GdiFlush();
		double sum[3] = {};
		for (int y = 0; y < cy; y++)
			for (int x = 0; x < cx; x++)
				for (int c = 0; c < 3; c++)
					sum[c] += ((unsigned char*)pix)[y * stride + x * 3 + c];
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s", g_logPath);
		if (char* slash = strrchr(path, '\\'))
			snprintf(slash + 1, path + sizeof(path) - slash - 1, "%s", file);
		BITMAPFILEHEADER fh = {};
		fh.bfType = 0x4D42;
		fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
		fh.bfSize = fh.bfOffBits + stride * cy;
		HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (f != INVALID_HANDLE_VALUE)
		{
			DWORD wr;
			WriteFile(f, &fh, sizeof(fh), &wr, NULL);
			WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &wr, NULL);
			WriteFile(f, pix, stride * cy, &wr, NULL);
			CloseHandle(f);
		}
		double px = (double)cx * cy;
		HWND fg = GetForegroundWindow();
		snprintf(buf, sizeof(buf), "foto da janela na tela (%dx%d em %ld,%ld, janela do jogo %sna frente): media RGB %.1f,%.1f,%.1f -> %s",
			cx, cy, p.x, p.y, fg == w ? "" : "NAO esta ", sum[2] / px, sum[1] / px, sum[0] / px, path);
		LogFile_Write("trava", buf);
		SelectObject(mem, old);
		DeleteObject(bm);
	}
	DeleteDC(mem);
	ReleaseDC(NULL, scr);
}

// Contorno da tela preta em placas AMD (liga sozinho nelas; COFMP_FRENTE=1 forca, =0 desliga): no notebook com Radeon integrada o jogo desenha certo no
// buffer de tras, mas a troca de buffers (wglSwapBuffers) nao chega na tela; desenhar direto no buffer da frente
// aparece. Entao o wglSwapBuffers do opengl32.dll que o jogo carregou vira uma copia do buffer de tras para o da
// frente (glBlitFramebuffer), sem trocar. O hw.dll chama essa funcao por ponteiro, entao o desvio e um jmp no
// comeco dela; a original nao e mais chamada.
typedef void(WINAPI* FrontBlit_t)(int, int, int, int, int, int, int, int, unsigned, unsigned);
static FrontBlit_t g_frontBlit;
static void(WINAPI* g_frontDrawBuffer)(unsigned);
static void(WINAPI* g_frontReadBuffer)(unsigned);
static void(WINAPI* g_frontGetIntegerv)(unsigned, int*);
static unsigned char(WINAPI* g_frontIsEnabled)(unsigned);
static void(WINAPI* g_frontDisable)(unsigned);
static void(WINAPI* g_frontEnable)(unsigned);
static void(WINAPI* g_frontFinish)();
static LONG g_frontFrames;

static BOOL WINAPI FrontSwapBuffers(HDC dc)
{
	RECT r;
	HWND w = WindowFromDC(dc);
	if (!w || !GetClientRect(w, &r) || r.right <= 0 || r.bottom <= 0)
		return TRUE;
	int draw = 0, read = 0;
	g_frontGetIntegerv(0x0C01, &draw);  // GL_DRAW_BUFFER
	g_frontGetIntegerv(0x0C02, &read);  // GL_READ_BUFFER
	bool scissor = g_frontIsEnabled(0x0C11) != 0;
	if (scissor)
		g_frontDisable(0x0C11);
	g_frontReadBuffer(0x0405);  // GL_BACK
	g_frontDrawBuffer(0x0404);  // GL_FRONT
	g_frontBlit(0, 0, r.right, r.bottom, 0, 0, r.right, r.bottom, 0x4000, 0x2600);  // GL_COLOR_BUFFER_BIT, GL_NEAREST
	g_frontDrawBuffer(draw);
	g_frontReadBuffer(read);
	if (scissor)
		g_frontEnable(0x0C11);
	// glFinish segura o jogo no ritmo da placa, ja que sem a troca de buffers nada mais segura
	g_frontFinish();
	g_frontFrames++;
	return TRUE;
}

static void FrontInstall()
{
	static bool tried;
	if (tried)
		return;
	const char* env = getenv("COFMP_FRENTE");
	if (env && *env == '0')
	{
		tried = true;
		return;
	}
	HMODULE gl = GetModuleHandleA("opengl32.dll");
	if (!gl)
		return;
	GL_FN(HGLRC, wglGetCurrentContext, ());
	GL_FN(PROC, wglGetProcAddress, (LPCSTR));
	GL_FN(const char*, glGetString, (unsigned));
	if (!wglGetCurrentContext || !wglGetProcAddress || !glGetString || !wglGetCurrentContext())
		return;  // tenta de novo no proximo frame
	tried = true;
	char path[MAX_PATH] = "", buf[MAX_PATH + 200];
	// liga sozinho em placas AMD (o defeito foi visto na Radeon integrada do Ryzen 5500U); COFMP_FRENTE=1 forca em
	// qualquer placa, COFMP_FRENTE=0 desliga
	const char* vendor = glGetString(0x1F00);  // GL_VENDOR
	bool amd = vendor && (strstr(vendor, "ATI") || strstr(vendor, "AMD"));
	if (!amd && !(env && *env == '1'))
		return;
	GetModuleFileNameA(gl, path, sizeof(path));
	g_frontBlit = (FrontBlit_t)wglGetProcAddress("glBlitFramebuffer");
	if (!g_frontBlit)
		g_frontBlit = (FrontBlit_t)wglGetProcAddress("glBlitFramebufferEXT");
	g_frontDrawBuffer = (void(WINAPI*)(unsigned))GetProcAddress(gl, "glDrawBuffer");
	g_frontReadBuffer = (void(WINAPI*)(unsigned))GetProcAddress(gl, "glReadBuffer");
	g_frontGetIntegerv = (void(WINAPI*)(unsigned, int*))GetProcAddress(gl, "glGetIntegerv");
	g_frontIsEnabled = (unsigned char(WINAPI*)(unsigned))GetProcAddress(gl, "glIsEnabled");
	g_frontDisable = (void(WINAPI*)(unsigned))GetProcAddress(gl, "glDisable");
	g_frontEnable = (void(WINAPI*)(unsigned))GetProcAddress(gl, "glEnable");
	g_frontFinish = (void(WINAPI*)())GetProcAddress(gl, "glFinish");
	BYTE* swap = (BYTE*)GetProcAddress(gl, "wglSwapBuffers");
	DWORD old;
	if (!g_frontBlit || !g_frontDrawBuffer || !g_frontReadBuffer || !g_frontGetIntegerv || !g_frontIsEnabled ||
		!g_frontDisable || !g_frontEnable || !g_frontFinish || !swap || !VirtualProtect(swap, 5, PAGE_EXECUTE_READWRITE, &old))
	{
		snprintf(buf, sizeof(buf), "copia para a frente: nao deu para ligar (blit %p, wglSwapBuffers %p em %s)", g_frontBlit, swap, path);
		LogFile_Write("trava", buf);
		return;
	}
	swap[0] = 0xE9;
	*(LONG*)(swap + 1) = (LONG)((BYTE*)FrontSwapBuffers - (swap + 5));
	VirtualProtect(swap, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), swap, 5);
	snprintf(buf, sizeof(buf), "copia para a frente ligada (placa %s; wglSwapBuffers de %s desviado)", vendor, path);
	LogFile_Write("trava", buf);
}

static void WatchBeat()
{
	InterlockedIncrement(&g_watchBeat);
	FrontInstall();
	LONG step = g_watchShot ? InterlockedExchange(&g_watchShot, 0) : 0;
	if (!step--)
		return;
	char buf[600];
	if (step == WATCH_STEPS)
	{
		// depois dos prints: teste do vermelho, e devolve os efeitos como estavam
		WatchGL(true);
		LogFile_Write("trava", "devolvendo os cvars como estavam");
		g_eng->pfnServerCommand(g_watchRestore);
		return;
	}
	if (step == 0)
	{
		WatchGL(false);
		int r = 0;
		for (const char* cv : g_watchCvars)
			if (r >= 0 && r < (int)sizeof(g_watchRestore) - 1)
				r += snprintf(g_watchRestore + r, sizeof(g_watchRestore) - r, "%s \"%s\";", cv, g_eng->pfnCVarGetString(cv));
		if (r > 0 && r < (int)sizeof(g_watchRestore) - 1)
			strcat(g_watchRestore, "\n");
		else
			g_watchRestore[0] = 0;
	}
	int n = snprintf(buf, sizeof(buf), "print %d (HalfLife%02d.tga), cvars:", step + 1, step);
	for (const char* cv : g_watchCvars)
		if (n > 0 && n < (int)sizeof(buf))
			n += snprintf(buf + n, sizeof(buf) - n, " %s=%s", cv, g_eng->pfnCVarGetString(cv));
	LogFile_Write("trava", buf);
	g_eng->pfnServerCommand((char*)"screenshot\n");
	if (step < WATCH_STEPS - 1)
	{
		snprintf(buf, sizeof(buf), "desligando para o proximo print: %s", g_watchSteps[step]);
		LogFile_Write("trava", buf);
		snprintf(buf, sizeof(buf), "%s\n", g_watchSteps[step]);
		g_eng->pfnServerCommand(buf);
	}
}

static int WatchModules(WatchMod* out, int max)
{
	HMODULE mods[512];
	DWORD need;
	if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need))
		return 0;
	int n = 0;
	for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512 && n < max; i++)
	{
		MODULEINFO mi;
		if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi)))
			continue;
		out[n].base = (DWORD)(size_t)mi.lpBaseOfDll;
		out[n].size = mi.SizeOfImage;
		if (!K32GetModuleBaseNameA(GetCurrentProcess(), mods[i], out[n].name, sizeof(out[n].name)))
			strcpy(out[n].name, "?");
		n++;
	}
	return n;
}

static int WatchName(char* buf, int size, const WatchMod* m, int n, DWORD a)
{
	for (int i = 0; i < n; i++)
		if (a - m[i].base < m[i].size)
			return _snprintf(buf, size, " %s+%x", m[i].name, a - m[i].base);
	return _snprintf(buf, size, " %x", a);
}

// a palavra da pilha parece endereco de retorno: aponta para codigo logo depois de uma instrucao call
static bool WatchIsReturn(DWORD a)
{
	MEMORY_BASIC_INFORMATION mb;
	if (a < 0x10000 || !VirtualQuery((void*)(size_t)(a - 7), &mb, sizeof(mb)) || mb.State != MEM_COMMIT ||
		!(mb.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
		a > (DWORD)(size_t)mb.BaseAddress + mb.RegionSize)
		return false;
	const BYTE* p = (const BYTE*)(size_t)a;
	return p[-5] == 0xE8 || (p[-6] == 0xFF && (p[-5] == 0x15 || (p[-5] & 0xF8) == 0x90)) ||
		(p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || (p[-1] & 0xF8) == 0x10)) || (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50);
}

static BOOL CALLBACK WatchWindow(HWND w, LPARAM lp)
{
	DWORD pid;
	GetWindowThreadProcessId(w, &pid);
	if (pid != GetCurrentProcessId())
		return TRUE;
	std::string* out = (std::string*)lp;
	char cls[64], buf[160];
	RECT r;
	if (!GetClassNameA(w, cls, sizeof(cls)))
		strcpy(cls, "?");
	GetWindowRect(w, &r);
	// so funcoes que nao mandam mensagem para a janela: funcionam mesmo com a thread dela travada
	_snprintf(buf, sizeof(buf), " [%s%s%s%s %dx%d em %d,%d]", GetParent(w) ? "filha " : "", cls,
		IsWindowVisible(w) ? "" : " invisivel", IsIconic(w) ? " minimizada" : "", r.right - r.left, r.bottom - r.top,
		r.left, r.top);
	buf[sizeof(buf) - 1] = 0;
	if (out->size() < 700)
		*out += buf;
	if (!GetParent(w))
		EnumChildWindows(w, WatchWindow, lp);
	return TRUE;
}

static void WatchReport(int secs, LONG frames)
{
	std::string wins;
	EnumWindows(WatchWindow, (LPARAM)&wins);
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	char cls[64] = "nenhuma";
	if (fg)
		GetClassNameA(fg, cls, sizeof(cls));
	char line[1000];
	_snprintf(line, sizeof(line), "%d s de jogo: servidor rodou %ld frames, %ld copias para a frente; em primeiro plano: %s (%s); janelas:%s",
		secs, frames, g_frontFrames, cls, pid == GetCurrentProcessId() ? "do jogo" : "de outro programa", wins.c_str());
	line[sizeof(line) - 1] = 0;
	LogFile_Write("trava", line);
}

static ULONGLONG WatchCpu()
{
	FILETIME c, e, k, u;
	if (!GetThreadTimes(g_mainThread, &c, &e, &k, &u))
		return 0;
	return ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

static void WatchSample(int secs, int cpu)
{
	static DWORD stack[16384]; // 64 KB a partir do ESP
	CONTEXT c;
	c.ContextFlags = CONTEXT_CONTROL;
	DWORD n = 0;
	if (SuspendThread(g_mainThread) == (DWORD)-1)
		return;
	BOOL ok = GetThreadContext(g_mainThread, &c);
	MEMORY_BASIC_INFORMATION mb;
	if (ok && VirtualQuery((void*)(size_t)c.Esp, &mb, sizeof(mb)) && mb.State == MEM_COMMIT)
	{
		n = ((DWORD)(size_t)mb.BaseAddress + (DWORD)mb.RegionSize - c.Esp) / 4;
		if (n > 16384)
			n = 16384;
		memcpy(stack, (void*)(size_t)c.Esp, n * 4);
	}
	ResumeThread(g_mainThread);
	if (!ok)
		return;
	static WatchMod mods[256];
	int nm = WatchModules(mods, 256);
	char line[1000];
	int len = _snprintf(line, sizeof(line), "sem frame do servidor ha %d s (thread principal usando %d%% de cpu). eip:", secs,
		cpu);
	len += WatchName(line + len, sizeof(line) - len, mods, nm, c.Eip);
	len += _snprintf(line + len, sizeof(line) - len, " pilha:");
	DWORD prev = 0;
	for (DWORD i = 0, found = 0; i < n && found < 28 && len < (int)sizeof(line) - 48; i++)
		if (stack[i] != prev && WatchIsReturn(stack[i]))
		{
			len += WatchName(line + len, sizeof(line) - len, mods, nm, stack[i]);
			prev = stack[i];
			found++;
		}
	line[sizeof(line) - 1] = 0;
	LogFile_Write("trava", line);
}

static DWORD WINAPI WatchThread(void*)
{
	LONG seen = g_watchBeat;
	DWORD since = GetTickCount();
	ULONGLONG cpu0 = WatchCpu();
	int next = 8;
	LogFile_Write("trava", "vigia de travamento ligado");
	static const int reportAt[] = { 30, 90, 180 };
	DWORD start = GetTickCount();
	LONG reportBeat = g_watchBeat;
	int report = 0;
	const char* shotEnv = getenv("COFMP_PRINT");
	int shots = shotEnv && *shotEnv == '1' ? 0 : WATCH_STEPS + 1;
	for (;;)
	{
		Sleep(1000);
		// primeiro print aos 20 s, depois um a cada 8 s; a ultima etapa (WATCH_STEPS) e o teste do vermelho
		if (shots <= WATCH_STEPS && GetTickCount() - start >= 20000u + shots * 8000u)
			InterlockedExchange(&g_watchShot, ++shots);
		if (report < 3 && GetTickCount() - start >= (DWORD)reportAt[report] * 1000)
		{
			LONG now = g_watchBeat;
			WatchReport(reportAt[report++], now - reportBeat);
			reportBeat = now;
		}
		LONG b = g_watchBeat;
		if (b != seen)
		{
			if (next > 8)
				LogFile_Write("trava", "voltou a rodar");
			seen = b;
			since = GetTickCount();
			cpu0 = WatchCpu();
			next = 8;
			continue;
		}
		DWORD ms = GetTickCount() - since;
		if (next <= 128 && ms >= (DWORD)next * 1000)
		{
			// tempo de cpu em unidades de 100 ns; ms * 10000 = tempo de relogio na mesma unidade
			int cpu = (int)((WatchCpu() - cpu0) * 100 / ((ULONGLONG)ms * 10000));
			WatchSample(ms / 1000, cpu);
			next *= 2;
		}
	}
}

static void WatchStart()
{
	if (g_mainThread || !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread,
		THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
		return;
	if (HANDLE t = CreateThread(NULL, 0, WatchThread, NULL, 0, NULL))
		CloseHandle(t);
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
		g_modDir = dir;
		LogFile_Open(dir);
		LogFile_Write("jogo", "---- cofmp v" COFMP_VERSION " carregado");
		WatchStart();
	}
	Hud_Start(HudCommand);
	static bool savePatched;
	if (!savePatched)
	{
		savePatched = true;
		InstallSavePatch();
	}
	if (!LoadHL())
		return;
	g_engForHL = *eng;
	g_engForHL.pfnSetView = Hook_SetView;
	g_engForHL.pfnCreateEntity = Hook_CreateEntity;
	g_engForHL.pfnCreateNamedEntity = Hook_CreateNamedEntity;
	g_engForHL.pfnRemoveEntity = Hook_RemoveEntity;
	g_engForHL.pfnFindEntityByString = Hook_FindEntityByString;
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
