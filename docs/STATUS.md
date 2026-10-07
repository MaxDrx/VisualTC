# Estado do projeto — VisualTC 0.4.0

Atualizado em 06/10/2026 (versão 0.4.0: idiomas português/espanhol/inglês, cor de destaque, botão Plano, fechar um estudo, menu do MPR e botão Cruz corrigidos, layout 1×3; antes, 0.3.0: MPR manipulado nas linhas, LUT colorida, linhas de referência e histograma corrigidos, 3D retirado). Convenção: um item só entra em **IMPLEMENTADO**
depois de compilado e testado (teste automatizado e/ou verificação visual
por captura de tela). A numeração (§) segue as seções do prompt mestre.

## IMPLEMENTADO

### Importação e organização (§ 4–8, 61, 77)
- Abrir arquivos, abrir pasta (recursiva), arrastar e soltar, linha de comando.
- Detecção pelo conteúdo (Part 10 e dataset cru), sem depender de extensão.
- Paciente → Estudo → Série → Instância, deduplicação por SOP Instance UID.
- Ordenação espacial (IPP · normal), sentido pela tendência do InstanceNumber.
- Divisão de séries por geometria, eco, TE, fase temporal, b-value, trigger,
  aquisição, Stack ID e Image Type; localizador de 3 planos; clipes de US.
- Análise de pilha: lacunas, espaçamento irregular, gantry tilt, duplicatas,
  FoR misto — avisos no painel e nas Informações DICOM.
- Arquivos inválidos, truncados, não suportados ou sem imagem: relatório de
  importação com motivo, sem interromper nada.
- Charsets ASCII, ISO_IR 100 (Latin-1), ISO_IR 192 (UTF-8) e UTF-8 não declarado.

### Exames compactados (0.2.0)
- ZIP (inclusive com senha ZipCrypto/AES), 7z, RAR, TAR/TGZ/TBZ/TXZ/TZST,
  GZ/BZ2/XZ/ZST de um arquivo e ISO 9660; aninhados até 3 níveis; detecção
  pelo conteúdo. Por Abrir, menu Arquivo, arrastar e soltar, linha de comando.
- Extração no processo isolado, nomes internos nunca usados como caminho,
  limites contra bombas de compressão e disco cheio, cancelamento.
- Pasta temporária por sessão, apagada ao fechar estudos/programa e, se o
  programa foi interrompido, na abertura seguinte.

### Interface (0.2.0)
- Painel de séries recolhível (« / Séries / F2) numa faixa de 36 px,
  redimensionável até só as miniaturas (com número da série), estado e
  largura lembrados; pode ir para a borda direita.
- Abertura pelo sistema: argumentos (inclusive unidade `D:\` vinda do
  Explorer) e `QFileOpenEvent` do macOS ("Abrir com", ícone no Dock).

### Novidades e correções (0.4.0)
- **Idiomas**: português (Brasil), espanhol e inglês (Preferências ›
  Interface), com tradutor próprio (`app/I18n`, tabelas
  `resources/i18n/*.json`) — sem ferramentas do Qt Linguist na compilação.
  As mensagens do núcleo (sem Qt) são traduzidas na exibição, inclusive as
  com partes variáveis. Teste `translations_complete` garante que nenhum
  texto fica sem tradução; textos padrão do Qt embutidos quando disponíveis.
- **Cor de destaque**: azul, sépia, amarelo, dourado, verde neon, laranja
  (Preferências ou Exibir › Cor de destaque), aplicada na hora.
- **Botão Plano**: axial → sagital → coronal na série do viewport ativo; o
  plano adquirido usa as imagens originais, os demais são reconstruídos (o
  volume é reaproveitado entre MPR e Plano).
- **Fechar um estudo**: × no cabeçalho do estudo no painel; libera viewports,
  MPR, cache e arquivos extraídos daquele estudo.
- **Menu do MPR** sem submenu (o de espessura não abria no Mac) e com cópia
  própria para o botão da barra; escolher MIP/espessura com o MPR fechado
  abre o MPR já com a opção.
- **Cruz** passa a mostrar/ocultar as linhas do MPR (X); posicionar o
  cruzamento virou Shift+X.
- Barra: "Cortes" → **Rolar**; botões Capturar e Reset retirados (funções
  continuam em Arquivo e Exibir); layout **1×3**.
- Sobre: "Criado por: Dr Marcelo Duarte - Brasil"; uso destinado a estudos e
  pesquisas.
- **Correção de robustez**: um decodificador isolado que tinha terminado
  enquanto ocioso (computador lento, sobrecarregado ou que voltou da
  suspensão) fazia a próxima imagem ser relatada como "corrompida" e nunca
  mais tentada. Agora o processo encerrado é detectado e substituído, e uma
  falha do processo tem uma nova tentativa com outro (um arquivo que de fato
  derruba o decodificador continua sendo relatado). Teste
  `isolatedDecoderSurvivesCrash` (fim silencioso numa thread sem event loop);
  achado pelo CI no runner macOS Intel.

### Correções e melhorias (0.3.0, após o teste no MacBook)
- **Linhas de referência**: aparecem também quando o topograma tem outro
  Frame of Reference (mesmo estudo); com uma só imagem na tela, ligar **Ref.**
  abre ao lado a série em outro plano (topograma primeiro); linha sólida e
  mais espessa (visível em Retina); mensagem na barra de status quando não há
  o que mostrar (ex.: duas séries paralelas).
- **MPR direto nas linhas**: arrastar a linha move só aquele plano; a bolinha
  na ponta gira os dois outros planos (oblíquo em qualquer ângulo, planos
  sempre perpendiculares); a barrinha ao lado define a espessura daquele
  plano (MIP/MinIP/média), com bordas tracejadas; o círculo central move o
  cruzamento. Cursores e dicas na tela; escala mantida durante a rotação;
  só o plano alterado é recalculado.
- **Tabela de cores (LUT)**: tons de cinza, ferro quente, PET, arco-íris,
  osso, cobre, fogo, gelo — botão LUT e menu Imagem; só exibição (HU e medidas
  inalterados); no MPR vale para os três planos.
- **Histograma da ROI**: habilitado assim que a ROI é desenhada ou
  selecionada; atalho Ctrl+Shift+H (no Mac, ⌘H escondia o aplicativo);
  também no menu do botão ROI.
- **Barra de ferramentas**: botões com espaço adequado (Window/Level,
  Medidas, ROI com seta de menu separada); em telas estreitas os botões menos
  usados mostram só o ícone em vez de sumirem atrás de "»"; atalhos nas dicas
  com a notação do sistema (⌘ no Mac).
- **Reconstrução 3D retirada** (decisão do usuário: leveza e fluidez); o MPR
  continua completo.

### Leitura de pixels (§ 9–11, 32–34, 74)
- Transfer syntaxes: Implicit/Explicit LE, Explicit BE, Deflate, JPEG
  Baseline/Extended, JPEG Lossless (14 e SV1), JPEG-LS (lossless e near),
  JPEG 2000 (lossless e lossy), RLE. Não suportadas (vídeo, HTJ2K, JPEG XL,
  J2K Part 2) com mensagem clara.
- 1/8/16/32 bits, com e sem sinal, BitsStored/HighBit com máscara e extensão
  de sinal; planar configuration 0/1; multiframe.
- MONOCHROME1/2, RGB, YBR_FULL, YBR_FULL_422 (nativo e JPEG, com detecção do
  espaço de cor no fluxo JPEG), YBR_PARTIAL_422, YBR_ICT/RCT, PALETTE COLOR.
- Enhanced CT/MR: functional groups por quadro (posição, orientação,
  espaçamento, rescale, VOI, eco, difusão, fase cardíaca).
- Rescale slope/intercept por quadro em double; VOI LINEAR, LINEAR_EXACT,
  SIGMOID; VOI LUT tabelada; janela automática por percentis.

### Visualização 2D (§ 12–27, 35–37, 46–47)
- Tema escuro de estação de trabalho (cores do § 15), imagem sobre preto, HiDPI.
- Barra de ferramentas completa (Abrir, Pasta, Séries, Layout, Zoom, Pan,
  W/L, Cortes, Medidas, ROI, Valor, MPR, Cruz, Girar, Espelhar, Inverter, LUT,
  Sincronizar, Ref., Anotações, Cine, Capturar, Exportar, Reset, Preferências).
- Painel de séries com miniaturas assíncronas.
- Scroll (roda, trackpad, teclado, arrasto), cine com FPS/loop/reverso.
- W/L interativo, 8 presets de TC, presets do arquivo e personalizados.
- Zoom (ajuste, 1:1, roda, arrasto), pan, rotação 90°/livre, espelhamento,
  inversão, com aviso no viewport; interpolação linear/vizinho.
- Overlays configuráveis (ON/OFF), letras de orientação calculadas dos
  vetores e que acompanham rotação/espelhamento; avisos de compressão com
  perdas e de calibração.
- Estado por série (janela, zoom, pan, corte) preservado ao trocar de série.

### Medidas (§ 28–31, 38)
- Régua, ângulo, Cobb, ROI retangular/elíptica/livre (área, média, DP,
  mínimo, máximo em HU), valor do pixel, histograma da ROI.
- Seleção, edição de pontos, mover medida e rótulo, desfazer/refazer,
  apagar uma/todas.
- Medidas em mm só com calibração real (PixelSpacing, ImagerPixelSpacing com
  aviso, US regions); caso contrário, pixels + "SEM CALIBRAÇÃO".

### Multiview (§ 39–41)
- Layouts 1×1, 1×2, 2×1, 2×2, 3×2, 3×3; maximizar com duplo clique.
- Sincronização espacial por Frame of Reference (corte anatomicamente mais
  próximo), opcionalmente zoom/pan e janela; aviso quando não é possível.
- Linhas de referência, inclusive sobre o topograma (scout lines) e entre
  séries do mesmo estudo com Frame of Reference diferente; abertura automática
  da série em outro plano quando só há uma imagem na tela.

### MPR (§ 48–55)
- Axial/coronal/sagital com crosshair interativo e scroll independente.
- Linhas guia manipuladas com o mouse: mover um plano, girar (oblíquo livre,
  também em passos de ±5° pelo teclado), espessura por plano, mover o centro.
- Thick slab: média, MIP, MinIP (1–500 mm), por plano ou nos três.
- Restauração dos planos ortogonais.
- Volume com origem por corte (tilt e espaçamento irregular corretos),
  lacunas vazias, int16 + rescale para economizar memória.

### Exportação e preferências (§ 42–45, 56–58)
- PNG, JPEG, TIFF do viewport, com/sem anotações e com opção de ocultar a
  identificação; captura para a área de transferência.
- Preferências: tema, fonte, botões do mouse, cache, threads, interpolação,
  isolamento, qualidade do MPR, overlays, sincronização, presets.
- Informações DICOM (paciente, série, geometria analisada, imagem).

### Segurança, privacidade e robustez (§ 62–70)
- Totalmente offline, sem telemetria; log local sem dados de paciente.
- Leitura e decodificação em processo isolado (`visualtc-worker`) com
  reinício automático; respostas validadas.
- Preflight estrutural (comprimentos, VR, fragmentos, delimitadores,
  cabeçalhos RLE) e validação do cabeçalho JPEG antes do GDCM.
- Limites de dimensão, quadros, bytes e arquivos (bombas de descompressão).
- RAII em todo o código; ASan/UBSan e clang-tidy no CI.
- Arquivos originais nunca são modificados.

### Engenharia (§ 78–97)
- CMake + presets + vcpkg com versões fixadas (triplets macOS com alvo 12.0);
  documentação completa em `docs/`.
- Distribuição: `.deb` e AppImage (runtime estático, sem libfuse2) gerados
  por `packaging/linux/make_packages.sh` — **executados e testados aqui**
  (abrem um ZIP e o exame de demonstração); instalador Windows por usuário,
  sem senha de administrador, com runtime do VC++ e menu "Abrir no VisualTC";
  DMG universal (Apple Silicon + Intel) com janela "arraste para
  Aplicativos"; assinatura/notarização opcionais por *secrets*; versão
  publicada com nomes fixos por tag `v*`; página de download (`site/`).

## EM DESENVOLVIMENTO
- Nenhum item em andamento. Versão 0.4.0 aguardando o teste do usuário no
  MacBook.

## FORA DO ESCOPO (decisão de 06/10/2026)
- **Reconstrução 3D** (volume rendering com VTK): retirada a pedido do
  usuário para manter o programa leve e fluido. O MPR (inclusive oblíquo e
  MIP/MinIP) atende ao objetivo do programa.

## A FAZER
- **CPR** (reconstrução curva) e curvas de centro de vaso.
- Visualizador completo de tags DICOM (dump de todos os elementos).
- Pixel data em ponto flutuante (Float/Double Float Pixel Data) e
  BitsAllocated 32 com rescale em float.
- PET: SUV (peso, dose, meia-vida); fusão PET/CT.
- Charsets ISO 2022 (japonês/coreano), GB18030, Latin-2, cirílico, grego,
  árabe, hebraico.
- Comparação de exames (estudos anteriores) com registro.
- Salvar/carregar medidas (Presentation State ou arquivo próprio) e
  relatório de medidas.
- PACS (C-FIND/C-MOVE/C-STORE, DICOMweb) com DCMTK.
- Certificados de assinatura (Apple Developer ID + notarização; Authenticode
  no Windows): o CI já assina quando os *secrets* existem; a obtenção dos
  certificados é decisão (e custo) do distribuidor.
- 7z e RAR com senha; arquivos divididos em volumes (`.z01`, `.part1.rar`).
- Pacote `.rpm` (Fedora/openSUSE usam hoje o AppImage).
- Hanging protocols e layouts por modalidade.
- Testes com acervo de arquivos reais de fabricantes (GE, Siemens, Philips,
  Canon, Fujifilm) além dos fixtures sintéticos.

## BUGS CONHECIDOS
| Descrição | Impacto | Mitigação |
|---|---|---|
| GDCM 3.0.24: leitura fora dos limites em `parsej2k_imp` (JPEG 2000) e estouro de heap no caminho JPEG-LS com arquivos corrompidos | o processo que decodifica pode cair | decodificação em processo isolado; o visualizador relata erro no arquivo e continua (teste `isolatedDecoderSurvivesCrash`). Com `--no-isolation` esses arquivos podem derrubar o app |
| GDCM: ponteiro nulo no codec JPEG quando a precisão do SOF é inválida; divisão por zero/leitura fora do cabeçalho no RLE | idem | corrigido antes do GDCM (validação de cabeçalho JPEG e RLE); 13 500 arquivos corrompidos sem falhas sob ASan |
| Charsets fora de Latin-1/UTF-8 aparecem como Latin-1 | nomes com caracteres errados | documentado; imagem não é afetada |
| Qt sem plataforma gráfica neste ambiente de desenvolvimento | interface verificada só offscreen aqui | CI executa em Windows/macOS/Ubuntu reais |
| Volumes acima do limite configurado não abrem em MPR | MPR indisponível nesses exames | mensagem clara; o limite pode ser ajustado |

## REVISÃO DE CÓDIGO (05/10/2026) — erros encontrados e corrigidos
Cada correção tem um teste que falhava antes e passa depois.

| # | Erro | Consequência antes da correção | Teste |
|---|---|---|---|
| 1 | Valores decimais (DS) lidos com `strtod`, que segue a localidade do sistema | Em computador configurado em português, sem o processo isolado: PixelSpacing, posição, espessura e rescale com decimais eram descartados (medidas em pixels, HU errados com intercept "-1024.0", MPR indisponível) | `DS parsing does not depend on the process locale` |
| 2 | Imagem que falha na decodificação era pedida de novo a cada repintura | Laço infinito de decodificação (533 tentativas em 0,8 s), reinício contínuo do decodificador isolado, CPU a 100 % e log crescendo | `failedDecodeIsNotRetriedInALoop` |
| 3 | Arquivo multiframe maior que o cache: o quadro visível era expulso pelos demais do mesmo arquivo | Decodificação em laço do arquivo inteiro (37 s para exibir um quadro); no MPR, o arquivo era decodificado uma vez por corte | `multiFrameLargerThanCacheDoesNotThrash` |
| 4 | Atalhos 1–8 registrados duas vezes (ações da janela e do menu de presets) | Depois de abrir o menu de presets uma vez, as teclas 1–8 paravam de funcionar (atalho ambíguo) | `mainWindowShortcutsAreUniqueAndPresetsWork` |
| 5 | Cache de renderização do viewport comparava ponteiros crus | Possível exibição dos pixels de outro corte se um quadro novo fosse alocado no mesmo endereço do anterior | revisão (reforço estrutural: referência compartilhada) |
| 6 | Pilhas com espaçamento de pixel diferente (dois FOVs na mesma série) eram intercaladas | Volume MPR com geometria errada | `Stacks with different pixel spacing are never merged` |
| 7 | Vetores de orientação aceitos com até 0,05 de não ortogonalidade, sem correção | Pequenos erros em posição do paciente, planos MPR e linhas de referência | `Nearly orthogonal orientation vectors are orthonormalized` |
| 8 | Na varredura estrutural, Pixel Data implícito que começasse com bytes de um "Item" era lido como sequência | Arquivo válido rejeitado como corrompido | `Implicit VR pixel data that starts like an item tag is not rejected` |
| 9 | Float Pixel Data (mapas paramétricos) relatado como "arquivo truncado" | Mensagem enganosa | `Float Pixel Data is reported as unsupported, not as truncated` |
| 10 | Arquivos truncados entravam na série | Corte com erro na pilha e falha ao montar o volume MPR | revisão do scanner |
| 11 | Calibração de US aceita de regiões que não são 2D | Medida em mm com calibração de região de Doppler/M-mode | revisão do parser |
| 12 | Duplo clique na lista de séries abria a série duas vezes | Trabalho duplicado | revisão |
| 13 | "Fechar estudos" com importação em andamento | As imagens da importação reapareciam na lista recém-limpa | revisão |
| 14 | Escolha da série aberta automaticamente | Podia abrir um topograma em vez da série principal | revisão |
| 15 | Relatório de problemas mostrava só a última importação; ponteiro estático na janela | Problemas anteriores perdidos | revisão |
| 16 | Log só era rotacionado na abertura | Uma sessão longa podia crescer o log sem limite | revisão |
| 17 | Decodificador isolado escrevia o protocolo no stdout comum | Uma biblioteca que imprimisse no stdout corromperia a resposta (o app trataria como falha) | revisão (canal privado) |
| 18 | ROI livre testava cada pixel contra todos os vértices | Lentidão ao ajustar janela com ROIs livres grandes | `Freehand ROI pixel membership matches the point-in-polygon rule` |

Verificado e **sem erro**: Planar Configuration = 1 declarada indevidamente em
JPEG/JPEG-LS/JPEG 2000 coloridos (teste `Compressed colour ignores a wrong
Planar Configuration` adicionado como proteção).

## DECISÕES DE ARQUITETURA
1. **GDCM como biblioteca DICOM principal** (BSD, codecs incluídos,
   tolerante); DCMTK reservado para rede. Ver ARCHITECTURE.md § B.
2. **Núcleo sem Qt** (`visualtc_core`): testável isoladamente, reaproveitável
   no worker e em ferramentas de linha de comando.
3. **Pipeline 2D e MPR próprio em CPU** em vez de VTK: controle exato da
   geometria (origem por corte, lacunas sem interpolação) e testes com
   resultado exato; sem dependência de OpenGL (o 3D foi retirado).
4. **Processo isolado para codecs**: a única proteção real contra falhas de
   memória em código de terceiros; o custo é a cópia dos pixels
   decodificados pelo pipe (um worker por thread, reaproveitado).
5. **Ordenação sempre espacial**; InstanceNumber só define o sentido.
6. **Nada é inventado**: sem calibração → pixels; sem orientação → sem
   letras; sem posição → sem sincronização/MPR.
7. **Imutabilidade dos objetos DICOM** e comunicação por sinais entre
   threads.
8. **Cache LRU por bytes** com prioridades de decodificação (visível >
   prefetch > miniatura > fundo).
9. **Espaço de cor do JPEG lido do fluxo**, não do cabeçalho DICOM (como a
   libjpeg), porque muitos equipamentos declaram RGB para fluxos YCbCr.
10. **Interface em português (pt-BR) por padrão**, números com vírgula.

## TESTES REALIZADOS
Ambiente: Linux x86_64 (2 núcleos, 7,8 GB), GCC, Qt 6.8.3 (offscreen),
GDCM 3.0.24, libarchive 3.8.7, Catch2 3.7.1.

| Conjunto | Resultado |
|---|---|
| `ctest` Release com `-Werror` (`linux-local`) | todos aprovados (casos Catch2 + suíte de interface + traduções completas); o teste de localidade roda com `pt_BR.UTF-8` instalada (no CI ela é gerada) |
| `ctest` Debug + ASan + UBSan | todos aprovados, sem erros do sanitizer |
| Interface (QtTest offscreen) | 25 testes: **botão Plano (sagital, coronal, volta ao original), layout 1×3 e barra sem Capturar/Reset**, **menu do MPR sem submenu (5 mm abre o MPR) e Cruz que oculta/mostra as linhas**, **× fecha só um estudo e ele pode ser reaberto**, **cores de destaque e Preferências com idiomas**, **espanhol (menus, plurais, mensagens do núcleo com partes variáveis, vírgula decimal)**, mais janela DICOM, arrasto W/L, roda/teclado, régua 50 mm, ROI em HU, sincronização espacial, crosshair do MPR, **linhas do MPR (mover plano, girar 30° com a linha seguindo o mouse, espessura de um só plano, centro)**, **planos independentes do MPR (espessura/versão por plano, 460 rotações sem perder a perpendicularidade)**, **LUT colorida**, **histograma habilitado pela ROI**, **linhas de referência com topograma de outro FoR**, queda do worker, falha sem laço de decodificação, multiframe maior que o cache, atalhos únicos e preset pela tecla 1, ZIP AES com senha errada e certa, limpeza da pasta temporária, painel de séries recolhível/estreito/lembrado, abertura por `QFileOpenEvent` |
| Exames compactados (`[archive]`) | detecção pelo conteúdo; ZIP, ZIP sem compressão, 7z, TGZ, TBZ2, TXZ, TZST, ISO, `.dcm.gz`; nomes hostis e link simbólico; aninhados; ZipCrypto e AES-256; bombas e limites; 600 arquivos compactados danificados; protocolo do worker rejeita caminhos fora do destino. App real: ZIP de 26 MB com 425 imagens aberto em ~1,9 s, pasta temporária apagada ao sair |
| Localidade | app sem processo isolado com `LC_ALL=pt_BR.UTF-8`: calibração, espessura, posição e MPR corretos |
| QA numérico | 1000 × 1 − 1024 = −24 HU; 100 px × 0,5 mm = 50 mm; VOI conforme PS3.3; MPR exato em fantomas lineares (axial, tilt, espaçamento irregular, oblíquo) |
| Fuzzing (ASan, em processo) | 3 000 arquivos nativos corrompidos + 15 900 JPEG/JPEG Lossless/RLE corrompidos: nenhuma falha |
| Isolamento | queda forçada do worker durante a decodificação: app continua e reinicia o worker |
| Verificação visual | capturas em `docs/screenshots/` (1×1, 2×2 com sincronização e medidas, 3×3 com todas as modalidades, MPR fino e MIP 20 mm) |
| Desempenho (`vtc_bench`, Release, 2 núcleos) | varredura de 425 arquivos: 21 ms; decodificação por imagem: nativo 0,5 ms, JPEG-LS 1,0 ms, J2K 4,5 ms, RLE 4,7 ms, US 40 quadros RGB 17 ms; W/L 0,1–0,9 ms; volume 400×400×221 em 234 ms; MPR plano fino 2–3 ms; MIP 20 mm 44–64 ms |
| Empacotamento | `make_packages.sh` executado aqui: `.deb` (extraído e executado fora do ambiente de compilação, abrindo um ZIP com série JPEG 2000) e AppImage (runtime estático, sem libfuse2; abriu o exame de demonstração em MPR). Fundo do DMG e página de download verificados por captura (Windows, Mac em modo escuro, Linux, celular). Instalador Windows e DMG **não executados aqui** (exigem Windows/macOS) — o CI os gera e testa |
| Windows / macOS | compilados e testados no CI (GitHub Actions: Windows 2022, macOS 15 arm64 e Intel); versão 0.2.0 instalada e usada pelo usuário no MacBook |
| Barra de ferramentas | capturas a 1280, 1470 e 1900 px de largura: todos os botões visíveis; rótulos de Window/Level, Medidas e ROI sempre presentes |
