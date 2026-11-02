# cofmp — campanha do Cry of Fear em co-op

Proxy do server DLL (`hl.dll`). O engine carrega `cofmp.dll`, que repassa todos os exports para `hl.dll` e adiciona a sincronização.

## O que faz (v1)
- Faz os `trigger_changelevel` do single-player funcionarem em multiplayer. O original sai cedo porque `IsDeathmatch()` sempre retorna 1 em MP.
- **Saída por toque:** espera todos os jogadores vivos entrarem no volume. Mostra "Aguardando jogadores na saida: X/Y" na tela.
- **Saída por "use"** (portas, botões): troca de mapa na hora para todos.
- **Chegada:** cada jogador aparece na mesma posição relativa ao `info_landmark`, como no SP. Jogadores sobrepostos são empurrados para um ponto livre. Quem estava longe (mais de 256 unidades) de quem passou pela saída chega ao lado dele. Isso cobre as portas de transição (`inter_door`), que teleportam só quem abriu para a sala de saída: sem isso, o outro jogador caía no spawn inicial do mapa novo.
- **Skin do Simon:** o co-op do CoF veste todos de policial. O instalador monta `models\player\simon\simon.mdl` com os arquivos do próprio jogo: a malha e as texturas do Simon (`models\cutscene\player.mdl`) sobre o esqueleto e as animações do `police1.mdl` (os dois usam o mesmo Bip01). O servidor troca o `model` do userinfo de cada jogador para `simon`. Cada PC precisa do arquivo, então o amigo também roda o instalador (a opção 4 já gera o modelo). Sem o arquivo, ele vê os outros de policial.
- **Inventário entre mapas:** em MP o jogador renasce vazio no mapa novo. Antes da troca, a DLL salva o que cada jogador vivo carrega e devolve tudo 1 s depois de ele nascer:
  - armas, na mesma ordem dos slots do inventário, com a munição do pente
  - munição de reserva
  - a arma que estava na mão
  - bateria da lanterna e `goodpoints`

  A devolução usa o `GiveNamedItem` do próprio jogo.
- **Chaves e itens compartilhados:** quando um jogador pega uma chave ou item (`item_key` etc.), todos os jogadores vivos recebem uma cópia, e aparece "X pegou: nome" na tela dos outros. Quando alguém gasta o item numa porta, ele some do inventário de todos. Os itens também vão para o próximo mapa junto com o inventário.
- **Cutscenes sincronizadas:** quando um `trigger_camera` liga a câmera para um jogador, todos os jogadores vivos veem a mesma câmera (e ficam parados, se o dono ficou). Quando ela desliga, a visão de todos volta. Funciona interceptando o `pfnSetView` da tabela do engine que o `hl.dll` recebe.
- **Revive, respawn e checkpoints:**
  - **Revive:** quando um aliado cai, basta chegar perto dele, olhar para o corpo e segurar USAR. Não precisa de item: o mod chama o RCP nativo do co-op (ou o `StartRevive` direto, se o jogo recusar). A seringa (`weapon_syringe`) continua funcionando. Ele levanta no lugar com 30 de vida e com o inventário de antes.
  - **Pular:** depois de `cofmp_selfrespawn_time` segundos (padrão 10), o jogador caído pode apertar pular para renascer ao lado de um companheiro vivo **com os seus itens**. O aviso do jogo diz "lose weapons", mas o mod devolve tudo.
  - **Inventário depois da morte:** a morte zera os slots do inventário e o renascer dá uma pistola nova. O mod refaz os slots exatamente como estavam (mesma posição, então o quick select continua certo) e apaga a pistola extra, para ela não se multiplicar.
  - **Corpo no chão:** enquanto o jogador está caído, o mod mantém a animação `death`/`crouch_death` andando até o fim.
  - **Tempo limite:** se ninguém reviver, ele renasce assim mesmo depois de `cofmp_respawn_time` segundos (padrão 30).
  - **Todos mortos:** o jogo reinicia o mapa e cada um recebe de volta o inventário da última foto, que a DLL tira a cada segundo.

## Instalar
**Jeito fácil:** rode `dist\CoF_Coop_Mod.exe` e escolha 1 (instalar), 2 (desinstalar) ou 3 (hospedar). Ele acha o jogo nas bibliotecas da Steam sozinho. Só o host precisa do mod; quem entra usa o jogo normal.

Manual:
1. Rode `build.bat`, que gera `build\cofmp.dll`.
2. Copie a DLL para `Cry of Fear\cryoffear\cl_dlls\`.
3. No `cryoffear\liblist.gam`, troque `gamedll "cl_dlls/hl.dll"` por `gamedll "cl_dlls/cofmp.dll"`. O instalador guarda o original em `cryoffear\Mod coop\liblist.gam.orig`.

Para desinstalar, restaure o `liblist.gam.orig`. Verificar os arquivos na Steam também desfaz a troca.

**Pasta `cryoffear\Mod coop` (v1.9):** o instalador cria essa pasta, e tudo do mod que pode ficar fora do lugar do jogo vai para lá:
- o log (`cofmp_log.txt` e `cofmp_log_anterior.txt`);
- os crash dumps (`cofmp_crash_<data>.dmp`);
- o backup `liblist.gam.orig`;
- o túnel, na subpasta `tunel` (`CoF_Tunel.exe`, `steam_api.dll` e os arquivos que ele cria).

Ficam fora só o que o jogo procura num lugar fixo: `cl_dlls\cofmp.dll` (o `gamedll` do `liblist.gam`, ao lado do `hl.dll` original), o próprio `liblist.gam` e `models\player\simon\simon.mdl`. Ao instalar ou atualizar, os arquivos das versões antigas (`cl_dlls\cofmp_tunel`, log, dumps e backup soltos em `cryoffear`) são levados para a pasta nova ou apagados. A opção 2 (desinstalar) apaga a pasta inteira.

### Atualização automática
Toda vez que abre, o `CoF_Coop_Mod.exe` consulta a última release do GitHub. Se a tag for mais nova que a versão dele, baixa os arquivos soltos da release para a própria pasta, troca os antigos e reabre já atualizado. Se o mod estava instalado, a DLL do jogo é trocada na hora (com o jogo fechado). Sem internet, ele segue com a versão que tem.

O `ATUALIZAR.bat`, que fica na mesma pasta, faz só isso sem abrir o menu: roda `CoF_Coop_Mod.exe --atualizar`. Esse comando atualiza o instalador e o túnel, troca a DLL do jogo (ou instala o mod, se ele não estiver instalado) e sai. O instalador não troca o próprio `.bat` na atualização, porque o `cmd` lê o arquivo aos poucos enquanto ele roda.

## Publicar uma versão
Precisa do Visual Studio 2022 Build Tools, do SDK do Half-Life em `..\deps\halflife` (`git clone https://github.com/ValveSoftware/halflife ..\deps\halflife`), do `pefile` (`py -m pip install pefile`), do jogo instalado e do `gh` logado.

```
release.bat 1.2 "o que mudou"
```

Ele grava a versão em `VERSION`, compila a DLL, o instalador e o túnel, monta `dist\CoF_Coop_Mod.zip`, faz commit e push e cria a release `v1.2` com o zip (para quem baixa a primeira vez) e os arquivos soltos (que o instalador usa para se atualizar).

## Jogar
O host abre um servidor com o mapa inicial da campanha, por exemplo:

```
cof.exe -game cryoffear -insecure +maxplayers 4 +map c_apartment1
```

Os outros jogadores abrem o Cry of Fear e digitam no console (tecla `'`) `connect <ip do host>`. Pela internet o host precisa liberar a porta UDP 27015 no roteador, ou os dois entram na mesma VPN (Radmin VPN, ZeroTier, Hamachi) e usam o IP dela.

### Janela do co-op no jogo (tecla Home)
Com o mod instalado, aperte **Home** em qualquer momento do jogo (menu ou partida).
- **Hospedar:** escolha o número de jogadores e o mapa (padrão: 2 e `c_intro`) e clique em "Criar servidor". A DLL roda `disconnect; sv_lan 1; sv_lan_rate 100000; maxplayers N; map X`, abre o túnel em modo host e mostra o código, que já vai para a área de transferência.
- **Entrar:** cole o código (se ele já estiver copiado, aparece sozinho) e clique em "Entrar". Quando o túnel do host responde, a DLL roda `rate 100000; cl_cmdrate 60; cl_updaterate 30; connect 127.0.0.1:27020`.
- **Trocar mapa (v1.10):** com o servidor já aberto, roda `cofmp_map <mapa>`: troca de mapa sem fechar o servidor, levando o inventário de cada jogador.
- **Começar agora:** roda `cofmp_go`, que solta a espera do grupo ou da cutscene (abaixo) sem aguardar os outros.
- **Entrar pela Steam (v1.11):** quando o código aparece, o host publica no rich presence `connect = -cofmpjoin <código>` e `status`. Isso usa o `steam_api.dll` que o jogo já iniciou (ISteamFriends013: SetRichPresence 36, ClearRichPresence 37, GetFriendRichPresence 38). Na lista de amigos aparece "Entrar no jogo". Esse texto sai quando o túnel do host fecha ou quando ele entra no jogo de outro.
  - **Jogo aberto:** a DLL registra o `GameRichPresenceJoinRequested_t` (337) e chama `SteamAPI_RunCallbacks` na thread do HUD. O jogo não chama esse RunCallbacks.
  - **Jogo fechado:** a Steam abre o jogo com o texto na linha de comando, e o `CoFLaunchApp.exe` repassa os argumentos ao `cof.exe`. A DLL lê `-cofmpjoin` 3 s depois que a janela aparece.
  - Nos dois casos, preenche o código, abre o Home e entra.
  - Se nada disso chegar, o Home vazio preenche sozinho o código de um amigo que está hospedando.

**Sem VAC (v1.8):** no `hw.dll`, o servidor é seguro quando não há `-insecure` na linha de comando e `sv_lan` é 0. Por isso o servidor do Home abre com `sv_lan 1`. O amigo chega pelo túnel como 127.0.0.1, que conta como LAN, e entra normalmente. Por IP direto da internet não entra com `sv_lan 1`: para isso use o instalador (opção 3), que abre o jogo com `-insecure`. Com `sv_lan 1`, a banda de cada cliente é limitada por `sv_lan_rate`, que vale 20000 por padrão; o Home sobe para 100000.

**Espera antes das cutscenes (v1.8):** no `ServerActivate`, a DLL procura os `trigger_once`/`trigger_multiple` cuja cadeia de alvos (`target`, chaves do `multi_manager`, `trigger_relay`, até 6 níveis) chega numa `trigger_camera`. Ficam de fora os gatilhos com `master` (podem estar travados) e os com a flag "no clients". Quem toca um desses primeiro fica congelado e imortal dentro dele, e o toque é segurado. Quando todos os humanos do servidor estão vivos a até 96 unidades do gatilho, o `Touch` é chamado com o primeiro que chegou. Quem está morto também conta, então o grupo espera essa pessoa renascer. O host pode soltar antes com `cofmp_go`. Enquanto uma `trigger_camera` está ligada, todos ficam imortais: `takedamage` vai para `DAMAGE_NO`, `FL_GODMODE` liga e a vida não cai. Os valores de antes voltam quando a câmera desliga. Desliga com `cofmp_cutscene_wait 0`. Com `2`, os bots também contam, para testar sozinho.

**Pular cutscene no co-op (v1.10):** no single player o `hl.dll` só pula a cena com Enter quando não é multiplayer, e o pulo é `host_framerate 2`, que o motor ignora em multiplayer. No co-op o Enter (com o jogo em primeiro plano) manda `cofmp_skip`; o servidor confere se há uma `trigger_camera` ligada sem a flag 0x400 ("não pode pular"), libera `host_framerate` no `hw.dll` (RVA 0x7ea45, `74 2E` → `90 90`) e põe 2 até a câmera desligar ou o mapa trocar.

**Log (v1.8):** o `cofmp.dll` e o túnel escrevem em `cryoffear\Mod coop\cofmp_log.txt` (até a v1.8, `cryoffear\cofmp_log.txt`), com as tags `[jogo]` e `[tunel]`. Passando de 5 MB, o arquivo vira `cofmp_log_anterior.txt`. O log tem:
- tudo o que sai como `[cofmp]` no console;
- conexões e comandos dos clientes (repetições seguidas viram uma linha só);
- a cada 10 s, ping e perda de cada jogador e os fps do servidor;
- frames do servidor com mais de 250 ms;
- do túnel, a cada 10 s: pacotes, bytes e pedaços enviados e recebidos e o estado P2P (relay ou direto, e a fila da Steam).
- itens (v1.11): no `inventorydrop N`, a arma do slot antes e depois do `hl.dll` (modelo, arquivo de inventário em +0x110, `iuser3`, `effects`, dono, posição), seguida por 10 s; quando um jogador encosta numa arma/item e algo muda, o item e os 7 slots antes e depois.
- itens (v1.13): a arma do `inventorydrop` é achada pelo arquivo do slot; se o `hl.dll` a tirou do jogador e não a deixou visível no chão, o mod a põe no chão ("o mod pos"); itens no chão a até 120 unidades de um jogador são vigiados e, quando saem do chão, o log mostra o item e os slots do jogador antes e depois.
- lanterna (v1.13): no comando `flashlight`, as flags, a visão (`m_iViewEntity`, +0x2038) e a bateria; fora de cutscene, uma visão presa em outra entidade (que esconde a lanterna da própria tela no `client.dll`) é devolvida ao jogador e logada.

**Espera do grupo (v1.5):** ao carregar um mapa, o servidor congela os jogadores e segura os `trigger_*` (toque e `trigger_auto`) até nascerem `maxplayers` humanos. Depois de uma troca de mapa, espera quantos estavam no mapa anterior, por no máximo `cofmp_start_timeout` segundos (90 por padrão). Sem isso, o host nascia sozinho e a cutscene de abertura do `c_intro` rodava antes do amigo entrar. Desliga com `cofmp_start_wait 0`. Quem chega com uma `trigger_camera` ativa passa a ver a câmera também.

**Painel nativo do co-op (v1.5):** o `client.dll` só desenha o painel "Nome - Health N - Ping N" com o ícone da seringa nos mapas `cof_*`. Em multiplayer, a DLL troca a string comparada por uma vazia, então o painel aparece também nos mapas da campanha (`c_*`).

Como funciona por dentro:
- A janela é Win32 e roda numa thread do `cofmp.dll`, que o engine carrega no processo do jogo já no menu (`hud.cpp`).
- Os comandos entram numa fila. Quem a esvazia na thread principal é a janela do jogo, que é subclassificada (`WM_HUD_CMD`), e também o `StartFrame`.
- O instalador copia o túnel para `cryoffear\Mod coop\tunel`. O túnel grava o código em `codigo.txt` ao lado dele.
- O túnel abre minimizado e entra num job object, então fecha junto com o jogo.
- Em tela cheia, o jogo pode minimizar enquanto a janela está aberta.

### Sem abrir porta: túnel pela Steam
O `CoF_Tunel.exe` (com o `steam_api.dll` ao lado) leva o tráfego do jogo pela rede P2P da Steam. Ele se identifica como o próprio Cry of Fear (appid 223710). A Steam passou a recusar o appid 480 (Spacewar), e todo mundo que joga o mod já tem o Cry of Fear. Não precisa de VPN nem de porta aberta.

Como o `cof.exe` também roda como 223710 e drena os callbacks da Steam com `Steam_BGetCallback`, o `P2PSessionRequest_t` pode não chegar ao túnel. Por isso o host chama `AcceptP2PSessionWithUser` a cada 2 s para os amigos que estão jogando o appid 223710 (`GetFriendGamePlayed`). Sem pedido pendente, esse Accept abre uma conexão para o amigo. Se a conexão falha, ele fica 1 min sem ser chamado, e a falha não aparece na janela. No canal P2P 1, o join manda `cofmp <versão>` até o host responder. Quando ele responde, o túnel grava `conectado.txt`, e a janela do Home só manda o `connect` depois disso (ou após 45 s). Os dois lados mostram a versão do outro.

Pacotes do jogo com mais de 1200 bytes, o limite do envio sem confirmação da Steam, vão em pedaços sem confirmação. Cada pedaço leva o cabeçalho `FD FF FF FF <id:2> <i:1> <n:1>`, e o outro túnel remonta o pacote. Até a v1.7 esses pacotes iam pela fila confiável e chegavam depois dos pacotes pequenos enviados depois deles. O netchan descartava os grandes por chegarem fora de ordem, e eram eles que levavam os comandos e eventos. Isso causava o lag em tudo que não era andar.
1. **Host:** no instalador, opção **3**. O jogo abre hospedando e o túnel abre junto, mostrando um código (já copiado) para mandar ao amigo.
2. **Amigo:** no instalador, opção **4**. Ele cola o código, o túnel conecta e o jogo abre com `connect 127.0.0.1:27020`. Se não entrar, é só repetir esse comando no console.

As duas janelas do túnel ficam abertas enquanto jogam. Também dá para rodar direto: `CoF_Tunel.exe host [porta do jogo]` ou `CoF_Tunel.exe join <código> [porta local]`.

### Crashes
Se o jogo crashar com o mod carregado (no PC de quem hospeda), o mod grava `cryoffear\Mod coop\cofmp_crash_<data>.dmp`. Mande esse arquivo para análise.

Se o jogo parar de rodar frames por 8 s ou mais, um vigia (thread separada) anota no log a pilha da thread principal e as janelas do jogo. Aos 30, 90 e 180 s ele também anota quantos frames o servidor rodou. Com `COFMP_PRINT=1` no ambiente, o vigia tira prints em etapas, desligando grupos de efeitos de tela, e testa a apresentação do OpenGL de quatro jeitos. Isso serve para diagnosticar tela preta.

### Tela preta em placas AMD
Na Radeon integrada (Ryzen 5 5500U, Adrenalin 26.5.2), o Cry of Fear fica com a tela preta depois da intro, mesmo sem o mod. O jogo desenha certo no buffer de trás, mas a imagem enviada pelo `wglSwapBuffers` do driver não chega à tela. Já desenhar direto no buffer da frente aparece. Quando a placa é AMD (`GL_VENDOR` com ATI ou AMD), o mod desvia o `wglSwapBuffers` do `opengl32.dll` carregado pelo jogo. Em vez de trocar os buffers, ele copia o buffer de trás para o da frente (`glBlitFramebuffer`) e chama `glFinish`. Com `COFMP_FRENTE=1` o desvio liga em qualquer placa; com `COFMP_FRENTE=0`, desliga.

### Roteiro de teste com 2 jogadores
O roteiro é o arquivo `cryoffear\Mod coop\teste.txt` ou o indicado em `COFMP_TESTE`, carregado ao abrir o jogo. Também dá para carregar com `cofmp_teste [arquivo]` no console do host. Cada linha tem o formato `segundos [jN] comando` e vai ao console do jogador N como se ele digitasse; as linhas do jogador 2 rodam no PC dele. A contagem começa quando todos os jogadores do roteiro estão vivos. O mod tem comandos próprios para o roteiro:
- `!olhar P`: muda o ângulo vertical da visão (positivo = para baixo).
- `!criar classe`: cria um item novo 48 unidades à frente.
- `!usar`: usa o item mais perto com o botão Usar apertado.
- `!aviso texto`: mostra o texto no meio da tela do jogador.

Apague o `teste.txt` depois do teste, senão ele roda em toda partida.

## Cvars e comandos (servidor)
| | |
|---|---|
| `cofmp_enable 1` | liga/desliga tudo |
| `cofmp_exit_wait 1` | 1 = saída por toque espera o grupo; 0 = troca assim que alguém toca |
| `cofmp_exit_timeout 0` | segundos até trocar mesmo sem todos (0 = espera sempre) |
| `cofmp_exits` | lista as saídas do mapa atual |
| `cofmp_tp_exit <#>` | leva todos os jogadores vivos para a saída `#` (destravar o grupo) |
| `cofmp_keep_inventory 1` | 1 = leva o inventário de cada jogador para o próximo mapa |
| `cofmp_share_items 1` | 1 = chaves/itens pegos por um jogador vão para todos |
| `cofmp_model simon` | modelo forçado em todos os jogadores (vazio = deixa o do jogo) |
| `cofmp_respawn_time 30` | segundos até o jogador caído renascer sozinho perto do grupo, se ninguém reviver (0 = só por revive ou pulando) |
| `cofmp_cutscene_wait 1` | 1 = gatilho de cutscene espera todos chegarem perto (quem chega fica parado e imortal); 0 = desliga; 2 = conta bots também (testes) |
| `cofmp_go` | solta a espera do começo do mapa ou da cutscene sem aguardar os outros (Home > Começar agora) |
| `cofmp_map <mapa>` | troca o servidor para outro mapa levando o inventário (Home > Trocar mapa) |
| `cofmp_skip` | pula a cutscene atual, como o Enter do single player (o Enter manda isso no co-op) |
| `cofmp_debug` | estado dos slots de jogador |
| `cofmp_inv` | chaveiro do grupo, armas, munição e slots de inventário de cada jogador |
| `cofmp_bot [nome]` | cria um bot parado (testes com 1 pessoa só) |
| `cofmp_use <classe> x y z [slot]` | o jogador do slot (padrão: o primeiro humano) usa a entidade dessa classe mais perto do ponto (testes) |
| `cofmp_after <seg> <comando>` | roda o comando depois de alguns segundos (roteiros de teste). Com `-cofmp_test <cfg>` na linha de comando, o cfg roda no primeiro mapa da campanha |
| `cofmp_kill <n>` / `cofmp_revive <n>` | derruba / revive (pelo revive nativo) o jogador do slot n (testes) |
| `cofmp_tp x y z` | leva o primeiro jogador para o ponto livre mais próximo (testes) |
| `cofmp_give <classe>` | dá um item a todos os jogadores vivos (testes) |
| `cofmp_select <classe>` | põe a arma na mão de todos os jogadores vivos (testes) |

## Notas de engenharia reversa
- O detour é aplicado em `CChangeLevel::ChangeLevelNow`, no RVA 0x109d40 do `hl.dll`. Antes de aplicar, a DLL confere a assinatura dos bytes; se o `hl.dll` mudar, o detour é desativado e um aviso aparece no console.
- Inventário: `CBasePlayer::GiveNamedItem` fica no RVA 0xe0a80 e `SelectLastItem` no RVA 0xf1c20. Os dois são conferidos por assinatura. Os offsets dos campos vieram das tabelas de save/restore: `m_rgpPlayerItems` 0x2514, `m_rgAmmo` 0x2540, `inventorySlot` 0x21fc e `m_iClip` 0x1e4.
- O cliente do CoF não repassa o comando `weapon_x`. Por isso a troca de arma é feita apontando `m_pLastItem` para a arma desejada e chamando `SelectLastItem`, que é o `lastinv` do jogo.
- Itens de inventário (`inventorySlot`, strings como `inventoryitems/apartmentkey.txt`): `AddToInventory` fica no RVA 0xdad40 (`thiscall(player, arquivo, modelo, classe)`; não duplica item) e `RemoveInventoryItem` no RVA 0xdda70 (`thiscall(player, arquivo, bool todos)`). A DLL compara os slots de cada jogador a cada 0,2 s: item novo entra no chaveiro do grupo e é dado a todos; item do chaveiro que sumiu foi gasto e é removido de todos.
- Abrir porta trancada: o cliente manda `inventoryuse <slot>`, que chama `CBasePlayer::UseInventoryItem` (0x100e4410). Ele procura um `inter_door` a até 64 unidades cujo `lockedby` (+0x18c) seja o nome do item, destranca e tira o item do slot. O "use" normal da porta só mostra a mensagem de trancada.
- O `entvars_t` do CoF tem 4 bytes extras depois de `light_level`, então `health` fica em 0x164. O layout corrigido está em `sdk_cof/progdefs.h`. Os offsets vieram de `gEntvarsDescription` no `hl.dll`.
- Portas de transição: `CInterDoor::Use` fica no RVA 0x48f70 (slot 58 da vtable). Ela escurece a tela de quem abriu e, 2 s depois, o think (0x48070) teleporta só esse jogador para o `inter_door_exit` do `target`. Nos mapas, essa sala de saída tem um `trigger_multiple` que dispara o `trigger_changelevel`, com quem passou como ativador.
- Bots (`FL_FAKECLIENT`) não têm canal de rede. Na troca de mapa o engine os derruba, e um `SetView` neles dá "SZ_GetSpace: Tried to write to an uninitialized sizebuf_t". Por isso a câmera de cutscene não é compartilhada com bots nem durante a troca.
- Revive nativo: `CBasePlayer::StartRevive` (RVA 0xf1860, `thiscall`, sem argumentos). Ele é chamado pela seringa quando ela acerta um jogador morto e o modo é co-op. Também é chamado quando alguém segura USE perto do corpo. A função põe `pev->fuser3 = 3`, e o think de morte (0x100e9ab0) levanta o jogador com 30 de vida quando o tempo acaba.
