// Instalador do cofmp (campanha do Cry of Fear em co-op).
// Leva o cofmp.dll embutido como recurso (RCDATA 1). Acha a instalacao do jogo pelas bibliotecas
// da Steam, copia a DLL para cryoffear\cl_dlls e troca o gamedll do liblist.gam (com backup).
// Ao abrir, se atualiza sozinho pela ultima release do GitHub (ver Update).

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <math.h>
#include "../logfile.h" // COFMP_DIR

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

// le uma linha sem BOM, \r e espacos nas pontas (entrada redirecionada vem com \r\n)
static bool ReadLine(std::string& out)
{
	if (!std::getline(std::cin, out))
		return false;
	if (out.compare(0, 3, "\xEF\xBB\xBF") == 0)
		out.erase(0, 3);
	size_t a = out.find_first_not_of(" \t\r"), b = out.find_last_not_of(" \t\r");
	out = a == std::string::npos ? "" : out.substr(a, b - a + 1);
	return true;
}

static const char* REL_GAME = "\\steamapps\\common\\Cry of Fear";

static bool Exists(const std::string& p)
{
	return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool ReadFile(const std::string& p, std::string& out)
{
	std::ifstream f(p, std::ios::binary);
	if (!f)
		return false;
	std::stringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

static bool WriteFile(const std::string& p, const std::string& data)
{
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	if (!f)
		return false;
	f << data;
	return (bool)f;
}

static std::string RegString(HKEY root, const char* key, const char* value)
{
	char buf[MAX_PATH] = {};
	DWORD size = sizeof(buf);
	if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, NULL, buf, &size) != ERROR_SUCCESS)
		return "";
	std::string s = buf;
	for (char& c : s)
		if (c == '/')
			c = '\\';
	return s;
}

// instalacao completa = tem o hl.dll e o liblist.gam (copias parciais em outras bibliotecas nao servem)
static bool IsGameDir(const std::string& dir)
{
	return Exists(dir + "\\cof.exe") && Exists(dir + "\\cryoffear\\liblist.gam") &&
		Exists(dir + "\\cryoffear\\cl_dlls\\hl.dll");
}

static std::string FindGame()
{
	std::vector<std::string> libs;
	std::string steam = RegString(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath");
	if (steam.empty())
		steam = RegString(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath");
	if (!steam.empty())
	{
		libs.push_back(steam);
		std::string vdf;
		if (ReadFile(steam + "\\steamapps\\libraryfolders.vdf", vdf))
		{
			// linhas do tipo:  "path"		"E:\\SteamLibrary"
			size_t at = 0;
			while ((at = vdf.find("\"path\"", at)) != std::string::npos)
			{
				size_t a = vdf.find('"', at + 6);
				size_t b = a == std::string::npos ? a : vdf.find('"', a + 1);
				if (b == std::string::npos)
					break;
				std::string p = vdf.substr(a + 1, b - a - 1), clean;
				for (size_t k = 0; k < p.size(); k++)
				{
					clean += p[k];
					if (p[k] == '\\' && k + 1 < p.size() && p[k + 1] == '\\')
						k++;
				}
				libs.push_back(clean);
				at = b;
			}
		}
	}
	libs.push_back("C:\\Program Files (x86)\\Steam");
	for (const std::string& lib : libs)
		if (IsGameDir(lib + REL_GAME))
			return lib + REL_GAME;
	return "";
}

static bool ExtractDll(const std::string& dest)
{
	HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(1), (LPCSTR)RT_RCDATA);
	HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
	if (!g)
		return false;
	std::string data((const char*)LockResource(g), SizeofResource(NULL, r));
	return WriteFile(dest, data);
}

static bool SetGameDll(const std::string& liblist, const char* from, const char* to)
{
	std::string s;
	if (!ReadFile(liblist, s))
		return false;
	size_t at = s.find(from);
	if (at == std::string::npos)
		return s.find(to) != std::string::npos; // ja esta como queremos
	s.replace(at, strlen(from), to);
	return WriteFile(liblist, s);
}

// ---------------------------------------------------------------- modelo do Simon
//
// O co-op do CoF so tem modelos de policial com as animacoes de jogador. O Simon (models/cutscene/
// player.mdl) usa o mesmo esqueleto Bip01, com a mesma pose de referencia, mais 4 ossos de ponta
// (Toe01, Finger12). Montamos models/player/simon/simon.mdl a partir dos arquivos do proprio jogo:
// o police1.mdl inteiro (ossos, sequencias, hitboxes) + o Simon anexado no fim, com o cabecalho
// apontando para as texturas e a malha do Simon. Os vertices dos ossos extras vao para o osso pai.

static int I32(const std::string& b, size_t o)
{
	int v;
	memcpy(&v, b.data() + o, 4);
	return v;
}

static void SetI32(std::string& b, size_t o, int v)
{
	memcpy(&b[o], &v, 4);
}

struct MdlBone
{
	std::string name;
	int parent;
	float value[6];
};

static std::vector<MdlBone> MdlBones(const std::string& b)
{
	std::vector<MdlBone> out;
	int n = I32(b, 140), ix = I32(b, 144);
	for (int k = 0; k < n; k++)
	{
		size_t o = ix + k * 112;
		MdlBone bone;
		bone.name = std::string(b.c_str() + o);
		bone.parent = I32(b, o + 32);
		memcpy(bone.value, b.data() + o + 64, sizeof(bone.value));
		out.push_back(bone);
	}
	return out;
}

static bool BuildSimonModel(const std::string& game)
{
	std::string models = game + "\\cryoffear\\models\\", pol, sim;
	if (!ReadFile(models + "player\\police1\\police1.mdl", pol) || !ReadFile(models + "cutscene\\player.mdl", sim) ||
		pol.size() < 244 || sim.size() < 244 || pol.compare(0, 4, "IDST") || sim.compare(0, 4, "IDST"))
		return false;

	std::vector<MdlBone> pb = MdlBones(pol), sb = MdlBones(sim);
	std::vector<int> remap(sb.size());
	std::vector<bool> extra(sb.size());
	std::vector<std::vector<float>> xf(sb.size()); // matriz 3x3 + translacao (osso extra -> pai)
	for (size_t k = 0; k < sb.size(); k++)
	{
		auto find = [&](const std::string& n) {
			for (size_t j = 0; j < pb.size(); j++)
				if (pb[j].name == n)
					return (int)j;
			return -1;
		};
		int j = find(sb[k].name);
		if (j >= 0)
		{
			for (int c = 0; c < 6; c++)
				if (fabsf(sb[k].value[c] - pb[j].value[c]) > 1e-3f)
					return false; // esqueleto diferente: nao da para trocar so a malha
			remap[k] = j;
			continue;
		}
		if (sb[k].parent < 0 || (j = find(sb[sb[k].parent].name)) < 0)
			return false;
		remap[k] = j;
		extra[k] = true;
		const float* v = sb[k].value;
		float cx = cosf(v[3]), sx = sinf(v[3]), cy = cosf(v[4]), sy = sinf(v[4]), cz = cosf(v[5]), sz = sinf(v[5]);
		xf[k] = { cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz,
				  cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz,
				  -sy, sx * cy, cx * cy,
				  v[0], v[1], v[2] };
	}

	const int B = (int)pol.size();
	std::string out = pol + sim;
	auto fix = [&](size_t o) { SetI32(out, o, I32(out, o) + B); };
	auto move = [&](size_t at, int bone, bool point) {
		float p[3], q[3];
		memcpy(p, out.data() + at, sizeof(p));
		const std::vector<float>& m = xf[bone];
		for (int r = 0; r < 3; r++)
			q[r] = m[r * 3] * p[0] + m[r * 3 + 1] * p[1] + m[r * 3 + 2] * p[2] + (point ? m[9 + r] : 0);
		memcpy(&out[at], q, sizeof(q));
	};

	int ntex = I32(sim, 180), tix = I32(sim, 184);
	for (int t = 0; t < ntex; t++)
		fix(B + tix + t * 80 + 76);
	int nbp = I32(sim, 204), bpix = I32(sim, 208);
	for (int bp = 0; bp < nbp; bp++)
	{
		size_t o = B + bpix + bp * 76;
		int nmodels = I32(out, o + 64);
		fix(o + 72);
		for (int m = 0; m < nmodels; m++)
		{
			size_t mo = I32(out, o + 72) + m * 112;
			int nmesh = I32(out, mo + 72), nverts = I32(out, mo + 80), nnorms = I32(out, mo + 92);
			for (int f : { 76, 84, 88, 96, 100 })
				fix(mo + f);
			size_t vinfo = I32(out, mo + 84), verts = I32(out, mo + 88), ninfo = I32(out, mo + 96),
				   norms = I32(out, mo + 100);
			for (int v = 0; v < nverts; v++)
			{
				int bone = (unsigned char)out[vinfo + v];
				if (bone >= (int)sb.size())
					return false;
				if (extra[bone])
					move(verts + v * 12, bone, true);
				out[vinfo + v] = (char)remap[bone];
			}
			for (int v = 0; v < nnorms; v++)
			{
				int bone = (unsigned char)out[ninfo + v];
				if (bone >= (int)sb.size())
					return false;
				if (extra[bone])
					move(norms + v * 12, bone, false);
				out[ninfo + v] = (char)remap[bone];
			}
			for (int me = 0; me < nmesh; me++)
			{
				size_t meo = I32(out, mo + 76) + me * 20;
				fix(meo + 4);
				fix(meo + 16);
			}
		}
	}

	// cabecalho: texturas, skins e corpo do Simon; o resto (ossos, sequencias...) do policial
	for (int f : { 184, 188, 200, 208 })
		SetI32(out, f, I32(sim, f) + B);
	for (int f : { 180, 192, 196, 204 })
		SetI32(out, f, I32(sim, f));
	memset(&out[8], 0, 64);
	memcpy(&out[8], "simon.mdl", 9);
	SetI32(out, 72, (int)out.size());

	CreateDirectoryA((models + "player\\simon").c_str(), NULL);
	return WriteFile(models + "player\\simon\\simon.mdl", out);
}

static void InstallSimon(const std::string& game)
{
	if (BuildSimonModel(game))
		printf("  Modelo do Simon pronto (models\\player\\simon\\simon.mdl).\n");
	else
		printf("  Aviso: nao consegui montar o modelo do Simon; os jogadores vao aparecer de policial.\n");
}

static const char* GAMEDLL_ORIG = "gamedll \"cl_dlls/hl.dll\"";
static const char* GAMEDLL_MOD = "gamedll \"cl_dlls/cofmp.dll\"";

static bool GameRunning()
{
	// pelo processo: uma janela do Explorer com o nome da pasta "Cry of Fear" tambem bateria pelo titulo
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32 pe = { sizeof(pe) };
	bool found = false;
	for (BOOL ok = Process32First(snap, &pe); ok && !found; ok = Process32Next(snap, &pe))
		found = !_stricmp(pe.szExeFile, "cof.exe");
	CloseHandle(snap);
	return found;
}

// ---------------------------------------------------------------- fps acima de 100 nos clientes
//
// O cofmp.dll ja destrava o fps no PC do host. Quem entra nao carrega o cofmp.dll; para esses PCs,
// o instalador aplica o mesmo patch no hw.dll em disco (74 2C -> EB 2C no teste do developer).
// A assinatura e unica no hw.dll atual. O backup fica em Mod coop\hw.dll.orig e a opcao 6 desfaz
// trocando o byte de volta; assim um hw.dll atualizado pela Steam nao e substituido por um backup velho.

static const char SIG_FPS_JE[] = { 0x74, 0x2C, (char)0xD9, 0x45, (char)0xFC, (char)0xDC, 0x1D };
static const char SIG_FPS_JMP[] = { (char)0xEB, 0x2C, (char)0xD9, 0x45, (char)0xFC, (char)0xDC, 0x1D };

static std::string FpsMarkerPath(const std::string& game)
{
	return game + "\\cryoffear\\" COFMP_DIR "\\fps.on";
}

static std::string FpsBackupPath(const std::string& game)
{
	return game + "\\cryoffear\\" COFMP_DIR "\\hw.dll.orig";
}

// -1 = nao achou; -2 = achou mais de uma vez (nao mexe); >= 0 = offset
static int FindSignature(const std::string& data, const char* sig, size_t n)
{
	int found = -1;
	for (size_t at = 0; (at = data.find(std::string(sig, n), at)) != std::string::npos; at++)
	{
		if (found >= 0)
			return -2;
		found = (int)at;
	}
	return found;
}

// grava num .new e troca de uma vez; se o hw.dll estiver como somente leitura, tira a flag e devolve depois
static bool WriteFileAtomic(const std::string& path, const std::string& data)
{
	std::string tmp = path + ".new";
	if (!WriteFile(tmp, data))
		return false;
	DWORD attr = GetFileAttributesA(path.c_str());
	if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))
		SetFileAttributesA(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
	if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		DeleteFileA(tmp.c_str());
		if (attr != INVALID_FILE_ATTRIBUTES)
			SetFileAttributesA(path.c_str(), attr);
		return false;
	}
	if (attr != INVALID_FILE_ATTRIBUTES)
		SetFileAttributesA(path.c_str(), attr);
	return true;
}

static bool PatchFps(const std::string& game, bool quiet)
{
	std::string hw = game + "\\hw.dll";
	if (!Exists(hw))
	{
		if (!quiet)
			printf("\n  Nao achei o hw.dll em %s.\n", game.c_str());
		return false;
	}
	if (GameRunning())
	{
		if (!quiet)
			printf("\n  Feche o Cry of Fear antes de alterar o hw.dll.\n");
		return false;
	}
	std::string data;
	if (!ReadFile(hw, data))
	{
		if (!quiet)
			printf("\n  Nao consegui ler o hw.dll.\n");
		return false;
	}
	int je = FindSignature(data, SIG_FPS_JE, sizeof(SIG_FPS_JE));
	int jmp = FindSignature(data, SIG_FPS_JMP, sizeof(SIG_FPS_JMP));
	if (je == -2 || jmp == -2)
	{
		if (!quiet)
			printf("\n  A assinatura do limite de 100 aparece mais de uma vez no hw.dll; nao alterei.\n");
		return false;
	}
	std::string mod = game + "\\cryoffear\\" COFMP_DIR;
	CreateDirectoryA(mod.c_str(), NULL);
	std::string marker = FpsMarkerPath(game), backup = FpsBackupPath(game);
	if (jmp >= 0)
	{
		if (!Exists(marker))
			WriteFile(marker, "cofmp fps unlock\n");
		if (!quiet)
			printf("\n  O FPS acima de 100 ja esta destravado neste PC.\n");
		return true;
	}
	if (je < 0)
	{
		if (!quiet)
			printf("\n  Nao reconheci esta versao do hw.dll (a assinatura do limite de 100 nao esta la).\n"
				   "  O jogo pode ter atualizado; o patch de FPS nao foi aplicado.\n");
		return false;
	}
	// guarda uma copia do hw.dll original; a opcao 6 desfaz o patch no byte, sem usar o backup
	if (!CopyFileA(hw.c_str(), backup.c_str(), FALSE))
	{
		if (!quiet)
			printf("\n  Nao consegui guardar o backup em %s.\n", backup.c_str());
		return false;
	}
	data[je] = (char)0xEB;
	if (!WriteFileAtomic(hw, data))
	{
		if (!quiet)
			printf("\n  Nao consegui gravar o hw.dll. Feche o jogo e tente de novo.\n");
		return false;
	}
	std::string check;
	if (!ReadFile(hw, check) || check.size() != data.size() || check[je] != (char)0xEB)
	{
		if (!quiet)
			printf("\n  O hw.dll nao ficou com o patch; o backup esta em %s.\n", backup.c_str());
		return false;
	}
	if (!WriteFile(marker, "cofmp fps unlock\n") && !quiet)
		printf("\n  O patch foi aplicado, mas nao consegui criar o marcador %s.\n", marker.c_str());
	if (!quiet)
		printf("\n  FPS acima de 100 destravado neste PC (patch no hw.dll).\n"
			   "  Backup do original: %s\n"
			   "  A Steam pode desfazer ao verificar os arquivos; rode esta opcao de novo depois.\n",
			backup.c_str());
	return true;
}

static bool RestoreFps(const std::string& game, bool quiet)
{
	std::string hw = game + "\\hw.dll";
	if (!Exists(hw))
	{
		if (!quiet)
			printf("\n  Nao achei o hw.dll em %s.\n", game.c_str());
		return false;
	}
	if (GameRunning())
	{
		if (!quiet)
			printf("\n  Feche o Cry of Fear antes de alterar o hw.dll.\n");
		return false;
	}
	std::string data;
	if (!ReadFile(hw, data))
	{
		if (!quiet)
			printf("\n  Nao consegui ler o hw.dll.\n");
		return false;
	}
	int je = FindSignature(data, SIG_FPS_JE, sizeof(SIG_FPS_JE));
	int jmp = FindSignature(data, SIG_FPS_JMP, sizeof(SIG_FPS_JMP));
	if (je == -2 || jmp == -2 || (je >= 0 && jmp >= 0))
	{
		if (!quiet)
			printf("\n  A assinatura do limite de 100 aparece mais de uma vez no hw.dll; nao alterei.\n");
		return false;
	}
	std::string marker = FpsMarkerPath(game), backup = FpsBackupPath(game);
	if (je >= 0)
	{
		DeleteFileA(marker.c_str());
		DeleteFileA(backup.c_str());
		if (!quiet)
			printf("\n  O limite de 100 ja esta normal neste PC.\n");
		return true;
	}
	if (jmp < 0)
	{
		if (!quiet)
			printf("\n  Nao reconheci esta versao do hw.dll; nao alterei.\n");
		return false;
	}
	data[jmp] = (char)0x74;
	if (!WriteFileAtomic(hw, data))
	{
		if (!quiet)
			printf("\n  Nao consegui gravar o hw.dll. Feche o jogo e tente de novo.\n");
		return false;
	}
	std::string check;
	if (!ReadFile(hw, check) || check.size() != data.size() || check[jmp] != (char)0x74)
	{
		if (!quiet)
			printf("\n  O hw.dll nao ficou com o limite de 100 de volta; o backup esta em %s.\n", backup.c_str());
		return false;
	}
	DeleteFileA(marker.c_str());
	DeleteFileA(backup.c_str());
	if (!quiet)
		printf("\n  Limite de 100 restaurado neste PC.\n");
	return true;
}

// se o usuario escolheu a opcao 5, reaplica depois de uma atualizacao do instalador ou da Steam
static void EnsureFpsPatch(const std::string& game)
{
	if (Exists(FpsMarkerPath(game)) && !GameRunning())
		PatchFps(game, true);
}

static void UnlockFpsMenu(const std::string& game)
{
	printf("\n  O host ja destrava o FPS acima de 100 sozinho. Use esta opcao nos PCs de quem entra.\n");
	printf("  Ela troca 1 byte do hw.dll (o teste que corta o fps em 100).\n");
	printf("  A Steam pode desfazer ao verificar os arquivos ou atualizar o jogo; rode de novo depois.\n");
	printf("  Nao use em servidores com VAC. Continuar? (s/N): ");
	std::string ans;
	ReadLine(ans);
	if (ans != "s" && ans != "S" && ans != "sim" && ans != "SIM")
	{
		printf("\n  Nada alterado.\n");
		return;
	}
	PatchFps(game, false);
}

static std::string ExeDir();

// a janela do jogo (tecla Home) abre o tunel de cryoffear\Mod coop\tunel
static const char* TUNNEL_FILES[] = { "CoF_Tunel.exe", "steam_api.dll" };
// o que o tunel cria na pasta dele
static const char* TUNNEL_TEMP[] = { "steam_appid.txt", "codigo.txt", "conectado.txt" };

static void DeleteMatching(const std::string& dir, const char* pattern)
{
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\" + pattern).c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			DeleteFileA((dir + "\\" + fd.cFileName).c_str());
	while (FindNextFileA(h, &fd));
	FindClose(h);
}

static void DeleteTunnel(const std::string& tun)
{
	for (const char* f : TUNNEL_FILES)
		DeleteFileA((tun + "\\" + f).c_str());
	for (const char* f : TUNNEL_TEMP)
		DeleteFileA((tun + "\\" + f).c_str());
	RemoveDirectoryA(tun.c_str());
}

// Ate a v1.8 o tunel ficava em cl_dlls\cofmp_tunel e o log, os crash dumps e o backup do liblist.gam
// soltos em cryoffear: leva o que der para a pasta Mod coop e apaga o resto.
static void MoveOldFiles(const std::string& cry)
{
	std::string mod = cry + "\\" COFMP_DIR;
	DeleteTunnel(cry + "\\cl_dlls\\cofmp_tunel");
	auto move = [&](const std::string& name, const std::string& to) {
		std::string from = cry + "\\" + name;
		if (Exists(from) && !MoveFileExA(from.c_str(), (mod + "\\" + to).c_str(), MOVEFILE_REPLACE_EXISTING))
			DeleteFileA(from.c_str());
	};
	move("cofmp_log_anterior.txt", "cofmp_log_anterior.txt");
	// a dll nova ja pode ter comecado um log na pasta nova: o antigo vira o anterior
	move("cofmp_log.txt", Exists(mod + "\\cofmp_log.txt") ? "cofmp_log_anterior.txt" : "cofmp_log.txt");
	if (!Exists(mod + "\\liblist.gam.orig"))
		move("liblist.gam.orig", "liblist.gam.orig");
	DeleteFileA((cry + "\\liblist.gam.orig").c_str());
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((cry + "\\cofmp_crash_*.dmp").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		std::vector<std::string> dumps;
		do
			dumps.push_back(fd.cFileName);
		while (FindNextFileA(h, &fd));
		FindClose(h);
		for (const std::string& d : dumps)
			move(d, d);
	}
}

// cria cryoffear\Mod coop, traz os arquivos das versoes antigas e copia o tunel
static void SetupModDir(const std::string& cry)
{
	std::string mod = cry + "\\" COFMP_DIR, to = cry + "\\" COFMP_TUNNEL_DIR, from = ExeDir();
	CreateDirectoryA(mod.c_str(), NULL);
	MoveOldFiles(cry);
	if (!Exists(from + "\\CoF_Tunel.exe"))
		return;
	CreateDirectoryA(to.c_str(), NULL);
	for (const char* f : TUNNEL_FILES)
		CopyFileA((from + "\\" + f).c_str(), (to + "\\" + f).c_str(), FALSE);
}

static void Install(const std::string& game)
{
	std::string cry = game + "\\cryoffear";
	if (GameRunning())
	{
		printf("\n  Feche o Cry of Fear antes de instalar.\n");
		return;
	}
	if (!ExtractDll(cry + "\\cl_dlls\\cofmp.dll"))
	{
		printf("\n  ERRO: nao consegui copiar o cofmp.dll para %s\\cl_dlls.\n", cry.c_str());
		return;
	}
	SetupModDir(cry);
	std::string lib = cry + "\\liblist.gam", orig = cry + "\\" COFMP_DIR "\\liblist.gam.orig";
	if (!Exists(orig))
		CopyFileA(lib.c_str(), orig.c_str(), TRUE);
	if (!SetGameDll(lib, GAMEDLL_ORIG, GAMEDLL_MOD))
	{
		printf("\n  ERRO: nao consegui ajustar o liblist.gam.\n");
		return;
	}
	printf("\n  Mod instalado! Arquivos do mod (log, tunel, backup do liblist.gam) em cryoffear\\" COFMP_DIR ".\n");
	printf("  No jogo, aperte Home para hospedar ou entrar num servidor.\n");
	InstallSimon(game);
	EnsureFpsPatch(game);
}

static void Uninstall(const std::string& game)
{
	std::string cry = game + "\\cryoffear";
	if (GameRunning())
	{
		printf("\n  Feche o Cry of Fear antes de desinstalar.\n");
		return;
	}
	// se o hw.dll nao puder ser restaurado, o marcador e o backup ficam para o usuario tentar de novo
	bool fpsRestored = RestoreFps(game, false);
	if (!SetGameDll(cry + "\\liblist.gam", GAMEDLL_MOD, GAMEDLL_ORIG))
	{
		printf("\n  ERRO: nao consegui ajustar o liblist.gam.\n");
		return;
	}
	DeleteFileA((cry + "\\cl_dlls\\cofmp.dll").c_str());
	MoveOldFiles(cry); // o que sobrou de versoes antigas vai para Mod coop e some junto
	std::string mod = cry + "\\" COFMP_DIR;
	DeleteTunnel(cry + "\\" COFMP_TUNNEL_DIR);
	for (const char* p : { "cofmp_log*.txt", "cofmp_crash_*.dmp", "liblist.gam.orig" })
		DeleteMatching(mod, p);
	if (fpsRestored)
	{
		DeleteFileA(FpsMarkerPath(game).c_str());
		DeleteFileA(FpsBackupPath(game).c_str());
	}
	RemoveDirectoryA(mod.c_str()); // so se ficou vazia
	DeleteFileA((cry + "\\models\\player\\simon\\simon.mdl").c_str());
	RemoveDirectoryA((cry + "\\models\\player\\simon").c_str());
	printf("\n  Mod removido. O jogo voltou ao normal.\n");
}

// o tunel (CoF_Tunel.exe + steam_api.dll) fica na mesma pasta do instalador
static bool StartTunnel(const std::string& args)
{
	char dir[MAX_PATH];
	GetModuleFileNameA(NULL, dir, MAX_PATH);
	*strrchr(dir, '\\') = 0;
	std::string exe = std::string(dir) + "\\CoF_Tunel.exe";
	if (!Exists(exe))
		return false;
	return (INT_PTR)ShellExecuteA(NULL, "open", exe.c_str(), args.c_str(), dir, SW_SHOWNORMAL) > 32;
}

static void Host(const std::string& game)
{
	std::string lib;
	ReadFile(game + "\\cryoffear\\liblist.gam", lib);
	if (lib.find(GAMEDLL_MOD) == std::string::npos)
	{
		printf("\n  Instale o mod primeiro (opcao 1).\n");
		return;
	}
	std::string n, map;
	printf("\n  Quantos jogadores (2-4) [4]: ");
	ReadLine(n);
	if (n.empty() || n.find_first_not_of("0123456789") != std::string::npos)
		n = "4";
	printf("  Mapa inicial [c_apartment1]: ");
	ReadLine(map);
	if (map.empty() || map.find_first_of(" \"&|;") != std::string::npos)
		map = "c_apartment1";
	std::string args = "-game cryoffear -insecure +sv_lan 0 +sv_lan_rate 100000 +maxplayers " + n + " +map " + map;
	ShellExecuteA(NULL, "open", (game + "\\cof.exe").c_str(), args.c_str(), game.c_str(), SW_SHOWNORMAL);
	if (StartTunnel("host"))
		printf("\n  Abrindo o servidor e o tunel da Steam. Mande para o seu amigo o codigo que aparece no tunel.\n");
	else
		printf("\n  Abrindo o servidor... Seu amigo entra pelo console (tecla ') com:  connect SEU_IP\n");
}

static void Join(const std::string& game)
{
	printf("\n  Cole o codigo que o host te passou: ");
	std::string code;
	ReadLine(code);
	if (code.empty() || code.find_first_not_of("0123456789") != std::string::npos)
	{
		printf("\n  Codigo invalido.\n");
		return;
	}
	InstallSimon(game); // o modelo dos outros jogadores e lido do PC de quem esta vendo
	if (!StartTunnel("join " + code))
	{
		printf("\n  Nao achei o CoF_Tunel.exe na pasta do instalador.\n");
		return;
	}
	Sleep(3000); // da tempo da Steam montar a conexao antes do jogo tentar conectar
	ShellExecuteA(NULL, "open", (game + "\\cof.exe").c_str(), "-game cryoffear +rate 100000 +cl_cmdrate 60 +connect 127.0.0.1:27020",
		game.c_str(), SW_SHOWNORMAL);
	printf("\n  Abrindo o jogo conectado ao host. Se ele nao entrar, digite no console:  connect 127.0.0.1:27020\n");
}

// ---------------------------------------------------------------- atualizacao automatica
//
// Consulta a ultima release do REPO. Se a tag (v1.2) for mais nova que COFMP_VERSION, baixa os arquivos
// soltos da release (o .zip e so para quem baixa pela primeira vez) para a pasta do instalador como .new.
// Com tudo baixado, renomeia os atuais para .old (o Windows deixa renomear exe/dll abertos), poe os novos
// no lugar e roda o instalador novo nesta mesma janela. A DLL do jogo e trocada por SyncDll.

#ifndef COFMP_VERSION
#define COFMP_VERSION "0"
#endif

static const char* REPO = "CrawlerD/cofmp";

static bool HttpGet(const std::string& url, std::string& out)
{
	std::wstring w(url.begin(), url.end());
	wchar_t host[256], path[2048];
	URL_COMPONENTS uc = { sizeof(uc) };
	uc.lpszHostName = host;
	uc.dwHostNameLength = _countof(host);
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = _countof(path);
	if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS)
		return false;
	HINTERNET s = WinHttpOpen(L"cofmp-updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
	HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		WINHTTP_FLAG_SECURE) : NULL;
	bool ok = false;
	if (r)
	{
		WinHttpSetTimeouts(s, 5000, 5000, 10000, 30000);
		DWORD status = 0, size = sizeof(status);
		if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
			WinHttpReceiveResponse(r, NULL) &&
			WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL) &&
			status == 200)
		{
			out.clear();
			char buf[16384];
			DWORD got;
			while ((ok = WinHttpReadData(r, buf, sizeof(buf), &got) != FALSE) && got)
				out.append(buf, got);
		}
	}
	if (r)
		WinHttpCloseHandle(r);
	if (c)
		WinHttpCloseHandle(c);
	if (s)
		WinHttpCloseHandle(s);
	return ok;
}

// todos os valores string de uma chave do JSON, na ordem (o da API do GitHub nao tem aspas escapadas nesses campos)
static std::vector<std::string> JsonStrings(const std::string& json, const std::string& key)
{
	std::vector<std::string> out;
	std::string k = "\"" + key + "\"";
	for (size_t at = 0; (at = json.find(k, at)) != std::string::npos;)
	{
		size_t a = json.find_first_not_of(" \t\r\n:", at + k.size());
		size_t b = a == std::string::npos || json[a] != '"' ? std::string::npos : json.find('"', a + 1);
		if (b == std::string::npos)
			break;
		out.push_back(json.substr(a + 1, b - a - 1));
		at = b;
	}
	return out;
}

struct ReleaseAsset
{
	std::string name, url, digest;
};

// valor string de uma chave dentro de um objeto JSON pequeno
static std::string JsonString(const std::string& obj, const std::string& key)
{
	std::string k = "\"" + key + "\"";
	size_t at = obj.find(k);
	if (at == std::string::npos)
		return "";
	size_t a = obj.find_first_not_of(" \t\r\n:", at + k.size());
	if (a == std::string::npos || obj[a] != '"')
		return "";
	std::string out;
	for (size_t i = a + 1; i < obj.size(); i++)
	{
		char c = obj[i];
		if (c == '"')
			break;
		if (c == '\\' && i + 1 < obj.size())
		{
			i++;
			switch (obj[i])
			{
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			default: out += obj[i]; break;
			}
			continue;
		}
		out += c;
	}
	return out;
}

// percorre o array "assets" da release e devolve nome, URL e digest de cada asset
static std::vector<ReleaseAsset> JsonAssets(const std::string& json)
{
	std::vector<ReleaseAsset> out;
	size_t at = json.find("\"assets\"");
	if (at == std::string::npos)
		return out;
	at = json.find('[', at);
	if (at == std::string::npos)
		return out;
	int depth = 0;
	bool inStr = false, esc = false;
	std::string obj;
	for (size_t i = at; i < json.size(); i++)
	{
		char c = json[i];
		if (inStr)
		{
			if (esc)
				esc = false;
			else if (c == '\\')
				esc = true;
			else if (c == '"')
				inStr = false;
			if (depth >= 2)
				obj += c;
			continue;
		}
		if (c == '"')
		{
			inStr = true;
			if (depth >= 2)
				obj += c;
			continue;
		}
		if (c == '{')
		{
			depth++;
			if (depth == 2)
				obj.clear();
			if (depth >= 2)
				obj += c;
			continue;
		}
		if (c == '}')
		{
			if (depth >= 2)
				obj += c;
			depth--;
			if (depth == 1)
			{
				ReleaseAsset a;
				a.name = JsonString(obj, "name");
				a.url = JsonString(obj, "browser_download_url");
				a.digest = JsonString(obj, "digest");
				if (!a.name.empty() && !a.url.empty())
					out.push_back(a);
				obj.clear();
			}
			continue;
		}
		if (c == '[')
		{
			depth++;
			continue;
		}
		if (c == ']')
		{
			depth--;
			if (depth == 0)
				break;
			continue;
		}
		if (depth >= 2)
			obj += c;
	}
	return out;
}

// ---------------------------------------------------------------- hash e assinatura da atualizacao
//
// Todo download e conferido pelo SHA-256 que a API do GitHub devolve para o asset. Se a release
// publicar SHA256SUMS + SHA256SUMS.sig e esta build tiver a chave publica abaixo, a assinatura RSA
// do manifesto e conferida antes; assim nem um release trocado no GitHub passa sem a chave privada.
// A chave privada nunca entra no repo: tools/gen_update_key.ps1 gera o par e tools/sign_release.ps1 assina.

static const char kUpdatePubKeyPem[] =
	"-----BEGIN PUBLIC KEY-----\n"
	"MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAr2u+KBxmFCq1+gKU6Y5q\n"
	"lR+9AHHWuY0JJuA54AL14CFORCYpoymPciLInHaakliC49pQ0lMIf5C4FW8kXsbv\n"
	"xs1DFEDWbQho422eOwMWGzPKP0mXTm1huAEJud1OsVtr7muISJBoOJVpYxcyfBU0\n"
	"Ss3luVUS7OXCB9H+lol0t25c+1ZNTHFOo1bGBcshsM3UoT2gmOZweR/gBYha3fmI\n"
	"T+/vuSSiICcTRn9cWymgcNoBHoAUXOqu2mRZEOPgGVCrwA8lz1qe/HGgo7YgJCEd\n"
	"9UPPoIQm2Ac8OyDrUHCvko1TN2WmUb69Hn/GCGlgHF7aFdJQAFt5rrX9J5nr5doV\n"
	"L6RrUrMD2r9Co+bdh5HHygZmbnxmiN6ydFtxItl3Gd5jreBlw90JKFdiu7JfK+1N\n"
	"CI0mmIraM86nQJE6SgAORFnkIdV/yUVpKv+/NexJwFeRbr4PBznNQ/kOMzsxolN3\n"
	"oYIi7CiXK8wn3nX984fRQzqZhcFlKtSF+nnTKF/8xzz9AgMBAAE=\n"
	"-----END PUBLIC KEY-----\n";

static bool Sha256Hex(const std::string& data, std::string& hex)
{
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	DWORD objLen = 0, hashLen = 0, cb = 0;
	std::vector<BYTE> obj, digest;
	bool ok = false;
	do
	{
		if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
			break;
		if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cb, 0) < 0)
			break;
		if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0) < 0)
			break;
		obj.resize(objLen);
		digest.resize(hashLen);
		if (BCryptCreateHash(alg, &hash, obj.data(), objLen, NULL, 0, 0) < 0)
			break;
		if (BCryptHashData(hash, (PUCHAR)data.data(), (ULONG)data.size(), 0) < 0)
			break;
		if (BCryptFinishHash(hash, digest.data(), hashLen, 0) < 0)
			break;
		static const char* hexd = "0123456789abcdef";
		hex.clear();
		for (BYTE b : digest)
		{
			hex += hexd[b >> 4];
			hex += hexd[b & 15];
		}
		ok = true;
	} while (0);
	if (hash)
		BCryptDestroyHash(hash);
	if (alg)
		BCryptCloseAlgorithmProvider(alg, 0);
	return ok;
}

static std::string LowerAscii(std::string s)
{
	for (char& c : s)
		if (c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';
	return s;
}

// SHA256SUMS no formato "hash  nome" (uma linha por arquivo)
static bool ParseManifest(const std::string& text, std::map<std::string, std::string>& out)
{
	std::istringstream f(text);
	std::string line;
	while (std::getline(f, line))
	{
		size_t a = line.find_first_not_of(" \t\r");
		if (a == std::string::npos || line[a] == '#')
			continue;
		size_t sp = line.find_first_of(" \t", a);
		if (sp == std::string::npos)
			continue;
		std::string hash = LowerAscii(line.substr(a, sp - a));
		size_t b = line.find_first_not_of(" \t", sp);
		if (b == std::string::npos)
			continue;
		std::string name = line.substr(b);
		while (!name.empty() && (name.back() == '\r' || name.back() == ' ' || name.back() == '\t'))
			name.pop_back();
		if (!name.empty() && name[0] == '*')
			name.erase(0, 1);
		if (hash.size() == 64 && !name.empty())
			out[name] = hash;
	}
	return !out.empty();
}

// RSA PKCS#1 v1.5 + SHA-256, com a chave publica em PEM (SubjectPublicKeyInfo)
static bool VerifyRsaSha256(const std::string& pubkeyPem, const std::string& data, const std::string& sig)
{
	if (pubkeyPem.empty() || sig.empty())
		return false;
	DWORD derLen = 0;
	if (!CryptStringToBinaryA(pubkeyPem.c_str(), 0, CRYPT_STRING_BASE64HEADER, NULL, &derLen, NULL, NULL) || !derLen)
		return false;
	std::vector<BYTE> der(derLen);
	if (!CryptStringToBinaryA(pubkeyPem.c_str(), 0, CRYPT_STRING_BASE64HEADER, der.data(), &derLen, NULL, NULL))
		return false;
	DWORD infoLen = 0;
	if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, der.data(), derLen, 0, NULL, NULL, &infoLen))
		return false;
	std::vector<BYTE> infoBuf(infoLen);
	if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, der.data(), derLen, 0, NULL, infoBuf.data(), &infoLen))
		return false;
	CERT_PUBLIC_KEY_INFO* info = (CERT_PUBLIC_KEY_INFO*)infoBuf.data();
	HCRYPTPROV prov = 0;
	HCRYPTKEY key = 0;
	HCRYPTHASH hash = 0;
	bool ok = false;
	if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
	{
		if (CryptImportPublicKeyInfo(prov, X509_ASN_ENCODING, info, &key))
		{
			if (CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash))
			{
				if (CryptHashData(hash, (BYTE*)data.data(), (DWORD)data.size(), 0))
				{
					// o CryptoAPI espera a assinatura RSA em little-endian (invertida em relacao ao PKCS#1
					// que o openssl gera no SHA256SUMS.sig)
					std::vector<BYTE> rev(sig.rbegin(), sig.rend());
					ok = CryptVerifySignatureA(hash, rev.data(), (DWORD)rev.size(), key, NULL, 0) != FALSE;
				}
			}
		}
	}
	if (hash)
		CryptDestroyHash(hash);
	if (key)
		CryptDestroyKey(key);
	if (prov)
		CryptReleaseContext(prov, 0);
	return ok;
}

static int SelfTest()
{
	std::string hex;
	if (!Sha256Hex("abc", hex) || hex != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
	{
		printf("selftest: SHA-256 falhou (%s)\n", hex.c_str());
		return 1;
	}
	printf("selftest: SHA-256 ok\n");
	return 0;
}

static int VerifySignatureCli(const std::string& pubPath, const std::string& dataPath, const std::string& sigPath)
{
	std::string pub, data, sig;
	if (!ReadFile(pubPath, pub) || !ReadFile(dataPath, data) || !ReadFile(sigPath, sig))
	{
		printf("verify-signature: nao consegui ler os arquivos\n");
		return 2;
	}
	bool ok = VerifyRsaSha256(pub, data, sig);
	printf("verify-signature: %s\n", ok ? "ok" : "falhou");
	return ok ? 0 : 1;
}

// usado no teste do parser: imprime nome e digest dos assets de um JSON salvo da API
static int DumpAssets(const std::string& path)
{
	std::string json;
	if (!ReadFile(path, json))
	{
		printf("dump-assets: nao consegui ler %s\n", path.c_str());
		return 2;
	}
	for (const ReleaseAsset& a : JsonAssets(json))
		printf("%s\t%s\n", a.name.c_str(), a.digest.c_str());
	return 0;
}

// "v1.10.2" > "1.9"
static bool NewerVersion(const std::string& tag, const std::string& cur)
{
	auto parse = [](const std::string& s) {
		std::vector<int> v;
		size_t at = s.find_first_of("0123456789");
		while (at != std::string::npos && at < s.size() && isdigit((unsigned char)s[at]))
		{
			v.push_back(atoi(s.c_str() + at));
			at = s.find_first_not_of("0123456789", at);
			if (at == std::string::npos || s[at] != '.')
				break;
			at++;
		}
		return v;
	};
	std::vector<int> a = parse(tag), b = parse(cur);
	if (a.empty())
		return false;
	for (size_t k = 0; k < a.size() || k < b.size(); k++)
	{
		int x = k < a.size() ? a[k] : 0, y = k < b.size() ? b[k] : 0;
		if (x != y)
			return x > y;
	}
	return false;
}

static std::string ExeDir()
{
	char dir[MAX_PATH];
	GetModuleFileNameA(NULL, dir, MAX_PATH);
	*strrchr(dir, '\\') = 0;
	return dir;
}

static void DeleteOld(const std::string& dir)
{
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\*.old").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
		DeleteFileA((dir + "\\" + fd.cFileName).c_str()); // o exe antigo ainda aberto fica para a proxima vez
	while (FindNextFileA(h, &fd));
	FindClose(h);
}

// o .old do instalador antigo so pode ser apagado depois que ele fechar (ele espera este terminar):
// um cmd escondido tenta de novo a cada segundo, por ate 1 minuto
static void DeleteOldLater(const std::string& dir)
{
	std::string old = "\"" + dir + "\\*.old\"";
	std::string cmd = "cmd.exe /c for /l %i in (1,1,60) do (ping -n 2 127.0.0.1 >nul & del /f /q " + old +
		" 2>nul & if not exist " + old + " exit)";
	STARTUPINFOA si = { sizeof(si) };
	PROCESS_INFORMATION pi;
	if (CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir.c_str(), &si, &pi))
	{
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
}

// true = atualizou e o instalador novo ja rodou; o main so precisa sair com o codigo em exitCode
static bool g_onlyUpdate; // --atualizar (ATUALIZAR.bat): atualiza o instalador e o mod do jogo e sai

static bool Update(int& exitCode)
{
	std::string dir = ExeDir();
	DeleteOld(dir);

	std::string json;
	if (!HttpGet(std::string("https://api.github.com/repos/") + REPO + "/releases/latest", json))
		return false; // sem internet ou sem release: segue com esta versao
	std::vector<std::string> tag = JsonStrings(json, "tag_name");
	if (tag.empty() || !NewerVersion(tag[0], COFMP_VERSION))
		return false;

	std::vector<ReleaseAsset> assets = JsonAssets(json);
	std::map<std::string, ReleaseAsset> byName;
	for (const ReleaseAsset& a : assets)
		byName[a.name] = a;

	std::vector<std::string> names;
	for (const ReleaseAsset& a : assets)
	{
		const std::string& name = a.name;
		std::string ext = name.size() > 4 ? name.substr(name.size() - 4) : "";
		if (name.empty() || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") !=
				std::string::npos || name[0] == '.' || !_stricmp(ext.c_str(), ".zip") || !_stricmp(ext.c_str(), ".rar") ||
			!_stricmp(ext.c_str(), ".bat") || name == "SHA256SUMS" ||
			name == "SHA256SUMS.sig") // o cmd le o .bat aos poucos: trocar o que esta rodando quebra ele
			continue;
		names.push_back(name);
	}
	bool hasExe = false;
	for (const std::string& n : names)
		hasExe |= !_stricmp(n.c_str(), "CoF_Coop_Mod.exe");
	if (!hasExe)
		return false;

	// hash esperado de cada arquivo: manifesto assinado ou, sem chave embutida, o digest da API do GitHub.
	// Com a chave embutida a assinatura e obrigatoria: aceitar o digest da API quando faltasse o manifesto
	// deixaria qualquer release adulterado passar so apagando os dois arquivos.
	std::map<std::string, std::string> hashes;
	auto sums = byName.find("SHA256SUMS");
	auto sumsSig = byName.find("SHA256SUMS.sig");
	if (kUpdatePubKeyPem[0])
	{
		std::string manifest, sig;
		if (sums == byName.end() || sumsSig == byName.end() ||
			!HttpGet(sums->second.url, manifest) || !HttpGet(sumsSig->second.url, sig) ||
			!VerifyRsaSha256(kUpdatePubKeyPem, manifest, sig) || !ParseManifest(manifest, hashes))
		{
			printf("\n  A release %s nao tem um SHA256SUMS assinado que confira. Nao vou atualizar.\n", tag[0].c_str());
			return false;
		}
	}
	else
	{
		for (const std::string& n : names)
		{
			auto it = byName.find(n);
			if (it == byName.end() || it->second.digest.compare(0, 7, "sha256:") != 0)
			{
				printf("\n  A release %s nao tem o hash SHA-256 de %s. Nao vou atualizar.\n", tag[0].c_str(), n.c_str());
				return false;
			}
			hashes[n] = LowerAscii(it->second.digest.substr(7));
		}
	}

	printf("\n  Nova versao %s disponivel (esta e a %s). Baixando...\n", tag[0].c_str(), COFMP_VERSION);
	for (const std::string& n : names)
	{
		std::string data, hex;
		auto h = hashes.find(n);
		if (h == hashes.end() || !HttpGet(byName.at(n).url, data) || data.empty() || !Sha256Hex(data, hex) ||
			LowerAscii(hex) != h->second)
		{
			printf("  O hash de %s nao confere. Nao vou atualizar.\n", n.c_str());
			for (const std::string& m : names)
				DeleteFileA((dir + "\\" + m + ".new").c_str());
			return false;
		}
		if (!WriteFile(dir + "\\" + n + ".new", data))
		{
			printf("  Nao consegui gravar %s. Continuando com a versao atual.\n", n.c_str());
			for (const std::string& m : names)
				DeleteFileA((dir + "\\" + m + ".new").c_str());
			return false;
		}
	}
	for (const std::string& n : names)
	{
		std::string p = dir + "\\" + n;
		DeleteFileA((p + ".old").c_str());
		if (Exists(p) && !MoveFileA(p.c_str(), (p + ".old").c_str()))
		{
			printf("  Nao consegui substituir %s (feche o tunel e abra de novo).\n", n.c_str());
			DeleteFileA((p + ".new").c_str());
			continue;
		}
		MoveFileA((p + ".new").c_str(), p.c_str());
		DeleteFileA((p + ".old").c_str()); // so falha se estiver aberto (este exe, ou o tunel rodando)
	}
	printf("  Atualizado para a %s!\n", tag[0].c_str());

	// o novo roda nesta janela; esperamos ele terminar para o console nao voltar para o cmd no meio
	std::string exe = dir + "\\CoF_Coop_Mod.exe", cmd = "\"" + exe + "\" --atualizado" + (g_onlyUpdate ? " --atualizar" : "");
	STARTUPINFOA si = { sizeof(si) };
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(exe.c_str(), &cmd[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi))
	{
		printf("  Feche e abra o CoF_Coop_Mod.exe de novo.\n\n  Aperte Enter para sair.");
		std::string line;
		ReadLine(line);
		exitCode = 0;
		return true;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	exitCode = (int)code;
	return true;
}

// com o mod instalado, troca a DLL do jogo se ela for diferente da que vem neste instalador (versao nova)
static void SyncDll(const std::string& game)
{
	std::string cry = game + "\\cryoffear", lib, cur;
	ReadFile(cry + "\\liblist.gam", lib);
	if (lib.find(GAMEDLL_MOD) != std::string::npos)
	{
		if (!GameRunning())
			SetupModDir(cry); // tunel novo que veio com a atualizacao
		HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(1), (LPCSTR)RT_RCDATA);
		HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
		bool same = g && ReadFile(cry + "\\cl_dlls\\cofmp.dll", cur) &&
			cur == std::string((const char*)LockResource(g), SizeofResource(NULL, r));
		if (g && !same)
		{
			printf("\n  O mod instalado no jogo e de outra versao. Atualizando...");
			Install(game);
		}
	}
	// mesmo sem o mod instalado (cliente que so usa a opcao 5), reaplica o patch de FPS
	EnsureFpsPatch(game);
}

int main(int argc, char** argv)
{
	SetConsoleOutputCP(CP_UTF8);
	printf("==============================================\n");
	printf("  Cry of Fear - campanha em co-op (cofmp) v%s\n", COFMP_VERSION);
	printf("==============================================\n");

	// o instalador que acabou de ser baixado nao procura de novo (tag e versao desencontradas dariam laco)
	bool updated = false, noUpdate = false, fpsMode = false, fpsOffMode = false, selftest = false;
	bool wantVerify = false, verifySig = false;
	std::string gameArg, pubPath, dataPath, sigPath, dumpPath;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--atualizado"))
			updated = true;
		else if (!strcmp(argv[i], "--atualizar"))
			g_onlyUpdate = true;
		else if (!strcmp(argv[i], "--no-update"))
			noUpdate = true;
		else if (!strcmp(argv[i], "--fps"))
			fpsMode = true;
		else if (!strcmp(argv[i], "--fps-off"))
			fpsOffMode = true;
		else if (!strcmp(argv[i], "--selftest"))
			selftest = true;
		else if (!strcmp(argv[i], "--dump-assets") && i + 1 < argc)
			dumpPath = argv[++i];
		else if (!strcmp(argv[i], "--game") && i + 1 < argc)
			gameArg = argv[++i];
		else if (!strcmp(argv[i], "--verify-signature"))
		{
			wantVerify = true;
			if (i + 3 < argc)
			{
				verifySig = true;
				pubPath = argv[++i];
				dataPath = argv[++i];
				sigPath = argv[++i];
			}
		}
	}
	if (selftest)
		return SelfTest();
	if (!dumpPath.empty())
		return DumpAssets(dumpPath);
	if (wantVerify)
	{
		if (!verifySig)
		{
			printf("uso: CoF_Coop_Mod.exe --verify-signature <pub.pem> <arquivo> <assinatura>\n");
			return 2;
		}
		return VerifySignatureCli(pubPath, dataPath, sigPath);
	}

	int exitCode;
	if (!updated && !noUpdate && !fpsMode && !fpsOffMode && Update(exitCode))
		return exitCode;
	if (updated) // os .old que a versao anterior deixou
	{
		DeleteOld(ExeDir());
		DeleteOldLater(ExeDir());
	}

	std::string game;
	if (!gameArg.empty())
	{
		game = gameArg;
		while (!game.empty() && (game.back() == '"' || game.back() == '\\' || game.back() == ' '))
			game.pop_back();
		if (!game.empty() && game.front() == '"')
			game.erase(0, 1);
	}
	else
		game = FindGame();
	if (game.empty())
	{
		printf("\n  Nao achei o Cry of Fear nas bibliotecas da Steam.\n");
		printf("  Cole a pasta do jogo (a que tem o cof.exe): ");
		ReadLine(game);
		while (!game.empty() && (game.back() == '"' || game.back() == '\\' || game.back() == ' '))
			game.pop_back();
		if (!game.empty() && game.front() == '"')
			game.erase(0, 1);
		if (!IsGameDir(game))
		{
			printf("\n  Essa pasta nao tem o Cry of Fear completo.\n\n  Aperte Enter para sair.");
			ReadLine(game);
			return 1;
		}
	}
	if (!IsGameDir(game))
	{
		printf("\n  Essa pasta nao tem o Cry of Fear completo.\n");
		return 1;
	}
	printf("\n  Jogo encontrado em: %s\n", game.c_str());
	if (fpsMode || fpsOffMode)
		return (fpsMode ? PatchFps(game, false) : RestoreFps(game, false)) ? 0 : 1;
	SyncDll(game);
	if (g_onlyUpdate)
	{
		if (GameRunning())
		{
			printf("\n  O Cry of Fear esta aberto, entao o mod dentro do jogo nao foi trocado.\n");
			printf("  Feche o jogo e rode o ATUALIZAR.bat de novo.\n");
			return 1;
		}
		std::string lib;
		ReadFile(game + "\\cryoffear\\liblist.gam", lib);
		if (lib.find(GAMEDLL_MOD) == std::string::npos)
			Install(game);
		else
			SetupModDir(game + "\\cryoffear");
		EnsureFpsPatch(game);
		printf("\n  Pronto: mod na versao %s.\n", COFMP_VERSION);
		return 0;
	}

	for (;;)
	{
		std::string lib;
		ReadFile(game + "\\cryoffear\\liblist.gam", lib);
		bool installed = lib.find(GAMEDLL_MOD) != std::string::npos;
		printf("\n  Estado: %s\n", installed ? "MOD INSTALADO" : "mod nao instalado");
		printf("\n  1) Instalar / atualizar o mod\n");
		printf("  2) Desinstalar o mod\n");
		printf("  3) Hospedar a campanha co-op agora\n");
		printf("  4) Entrar no jogo de um amigo (tunel Steam)\n");
		printf("  5) Destravar o FPS acima de 100 neste PC (quem entra)\n");
		printf("  6) Restaurar o limite de 100 neste PC\n");
		printf("  0) Sair\n\n  Escolha: ");
		std::string opt;
		if (!ReadLine(opt) || opt == "0")
			break;
		if (opt == "1")
			Install(game);
		else if (opt == "2")
			Uninstall(game);
		else if (opt == "3")
			Host(game);
		else if (opt == "4")
			Join(game);
		else if (opt == "5")
			UnlockFpsMenu(game);
		else if (opt == "6")
			RestoreFps(game, false);
	}
	return 0;
}
