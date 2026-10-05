# Estado do projeto — VisualTC 0.2.0

Atualizado em 05/10/2026 (versão 0.2.0: exames compactados, painel de séries recolhível, instaladores de um clique e Mac Intel). Convenção: um item só entra em **IMPLEMENTADO**
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
- Barra de ferramentas completa (Abrir, Pasta, Estudos, Layout, Zoom, Pan,
  W/L, Medidas, ROI, MPR, 3D [desabilitado], Reset, Preferências etc.).
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
- Linhas de referência, inclusive sobre o topograma (scout lines).

### MPR (§ 48–55)
- Axial/coronal/sagital com crosshair interativo e scroll independente.
- Thick slab: média, MIP, MinIP (1–500 mm).
- Planos oblíquos em passos de ±5° e restauração dos ortogonais.
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
- **Validação nos runners do CI**: o instalador do Windows, o DMG universal
  e a página no GitHub Pages estão configurados (com testes de instalação no
  próprio CI), mas não foram executados neste ambiente, que é Linux sem
  acesso ao GitHub Actions (ver "Testes realizados").
- MPR oblíquo por arrasto livre dos eixos do crosshair (hoje: passos de 5°
  por atalho/menu).

## A FAZER
- **3D (v1.0)** com VTK 9.3: volume rendering, presets (osso, vasos,
  pele), MIP 3D, clipping, cropping, rotação/zoom 3D.
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
- Tradução da interface para inglês/espanhol (strings já em `tr()`).
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
   resultado exato; VTK entra apenas no 3D.
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
| `ctest` Release com `-Werror` (`linux-local`) | 94/94 aprovados (93 casos Catch2, 14 669 verificações, + suíte de interface); o teste de localidade roda com `pt_BR.UTF-8` instalada (no CI ela é gerada) |
| `ctest` Debug + ASan + UBSan | 94/94 aprovados, sem erros do sanitizer (suíte de interface repetida 6 vezes sem falhas) |
| Interface (QtTest offscreen) | 15 testes: janela DICOM, arrasto W/L, roda/teclado, régua 50 mm, ROI em HU, sincronização espacial, crosshair do MPR, queda do worker, falha sem laço de decodificação, multiframe maior que o cache, atalhos únicos e preset pela tecla 1, ZIP AES com senha errada e certa, limpeza da pasta temporária, painel de séries recolhível/estreito/lembrado, abertura por `QFileOpenEvent` |
| Exames compactados (`[archive]`) | detecção pelo conteúdo; ZIP, ZIP sem compressão, 7z, TGZ, TBZ2, TXZ, TZST, ISO, `.dcm.gz`; nomes hostis e link simbólico; aninhados; ZipCrypto e AES-256; bombas e limites; 600 arquivos compactados danificados; protocolo do worker rejeita caminhos fora do destino. App real: ZIP de 26 MB com 425 imagens aberto em ~1,9 s, pasta temporária apagada ao sair |
| Localidade | app sem processo isolado com `LC_ALL=pt_BR.UTF-8`: calibração, espessura, posição e MPR corretos |
| QA numérico | 1000 × 1 − 1024 = −24 HU; 100 px × 0,5 mm = 50 mm; VOI conforme PS3.3; MPR exato em fantomas lineares (axial, tilt, espaçamento irregular, oblíquo) |
| Fuzzing (ASan, em processo) | 3 000 arquivos nativos corrompidos + 15 900 JPEG/JPEG Lossless/RLE corrompidos: nenhuma falha |
| Isolamento | queda forçada do worker durante a decodificação: app continua e reinicia o worker |
| Verificação visual | capturas em `docs/screenshots/` (1×1, 2×2 com sincronização e medidas, 3×3 com todas as modalidades, MPR fino e MIP 20 mm) |
| Desempenho (`vtc_bench`, Release, 2 núcleos) | varredura de 425 arquivos: 21 ms; decodificação por imagem: nativo 0,5 ms, JPEG-LS 1,0 ms, J2K 4,5 ms, RLE 4,7 ms, US 40 quadros RGB 17 ms; W/L 0,1–0,9 ms; volume 400×400×221 em 234 ms; MPR plano fino 2–3 ms; MIP 20 mm 44–64 ms |
| Empacotamento | `make_packages.sh` executado aqui: `.deb` (extraído e executado fora do ambiente de compilação, abrindo um ZIP com série JPEG 2000) e AppImage (runtime estático, sem libfuse2; abriu o exame de demonstração em MPR). Fundo do DMG e página de download verificados por captura (Windows, Mac em modo escuro, Linux, celular). Instalador Windows e DMG **não executados aqui** (exigem Windows/macOS) — o CI os gera e testa |
| Windows / macOS | **não compilados neste ambiente**; configurados no CI (GitHub Actions) |
