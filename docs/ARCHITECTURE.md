# Arquitetura do VisualTC

Este documento responde às seções A–G do prompt mestre: arquitetura, escolha
de tecnologias, estrutura de diretórios, dependências, roadmap, critérios de
aceite e riscos técnicos. O estado de cada item está em [STATUS.md](STATUS.md).

## A. Visão geral

O VisualTC é dividido em três camadas com dependências em um único sentido:

```
┌──────────────────────────────────────────────────────────────────────────┐
│  VisualTC (executável Qt 6 Widgets)                                      │
│  app/  main, tema escuro, preferências (QSettings)                        │
│  ui/   MainWindow, SeriesBrowser, ViewerGrid, diálogos                   │
│  viewer2d/  Viewport (render, ferramentas, overlays), ImageSource,       │
│             StackSource, Annotations + AnnotationStore (undo/redo)       │
│  mpr/  MprSession, MprSource (ImageSource de planos reconstruídos)       │
│  io/   ImportTask, FrameProvider (fila priorizada), ThumbnailProvider,   │
│        DecoderClient ──────────────┐                                     │
│  export/ ImageExporter             │ pipes (protocolo binário validado)  │
└────────────────────────────────────┼─────────────────────────────────────┘
                                     ▼
┌──────────────────────────────────────────────────────────────────────────┐
│  visualtc-worker (processo isolado, 1 por thread de decodificação)       │
│  lê cabeçalhos e decodifica pixels com GDCM; sem core dump; limite RAM   │
└────────────────────────────────────┬─────────────────────────────────────┘
                                     │ usa
┌────────────────────────────────────▼─────────────────────────────────────┐
│  visualtc_core (biblioteca C++20 estática, SEM Qt, testável isolada)     │
│  dicom/  Preflight → Parser → Scanner → Sorter/Geometry → StudyDatabase  │
│          Decoder, WorkerProtocol, TextUtil (charsets, DS/IS, PN, datas)  │
│  imaging/ PixelData (DecodedFrame), WindowLevel (VOI/LUT/presets),       │
│           Orientation (letras R/L/A/P/H/F), FrameCache (LRU por bytes)   │
│  measurements/ MeasurementMath (mm, graus, Cobb), RoiStatistics (HU)     │
│  mpr/  ImageVolume (origem por corte), Reslicer (fino, MIP/MinIP/média), │
│        MprGeometry (eixos anatômicos, planos)                            │
│  synchronization/ SpatialSync (FoR, plano mais próximo, reference lines) │
│  core/ Logger (sem PHI), SystemInfo, Parallel, Vec3, PathUtil            │
└──────────────────────────────────────────────────────────────────────────┘
```

### Fluxo de abertura de um exame

1. **ImportTask** (thread própria) chama o `DicomScanner`: percorre a pasta
   (sem seguir links simbólicos de diretório, ignorando arquivos ocultos),
   e envia cada arquivo ao worker para `parse`. A detecção é pelo conteúdo
   (preâmbulo `DICM`, ou dataset cru sem preâmbulo), nunca pela extensão.
2. No worker, **DicomPreflight** percorre a estrutura do arquivo sem alocar
   valores: comprimentos maiores que o arquivo, VR inválidas, fragmentos
   corrompidos, delimitadores com comprimento ≠ 0 e cabeçalhos RLE inválidos
   tornam o arquivo "corrompido" antes que o GDCM o toque.
3. **DicomParser** lê o cabeçalho até `(7FE0,0010)` com GDCM e produz um
   `InstanceInfo` imutável: identificação, charset convertido para UTF-8,
   geometria por quadro (inclusive Enhanced CT/MR via functional groups),
   rescale, janelas, VOI LUT, calibração de US, flags de compressão com
   perdas e truncamento.
4. **StudyDatabase** agrupa Paciente → Estudo → Série → Instância
   (deduplicação por SOP Instance UID) e o **DicomSorter** divide e ordena
   cada série em pilhas espacialmente coerentes (ver "Geometria").
5. A UI mostra a árvore com miniaturas (ThumbnailProvider, prioridade
   Thumbnail) e abre automaticamente a maior série não-localizadora.
6. O **FrameProvider** decodifica sob demanda em um pool de threads com fila
   por prioridade (Visível 10 > Prefetch 5 > Miniatura 2 > Fundo 0); cada
   thread fala com seu próprio worker. Os quadros decodificados vão para o
   **FrameCache** LRU com orçamento em bytes (padrão 25 % da RAM, entre
   256 MiB e 2 GiB).
7. O **Viewport** aplica rescale → VOI → (inversão) via LUT de 8 bits
   pré-calculada (`DisplayRenderer`) e desenha com `QPainter`.

### Geometria (o centro do projeto)

- `ImagePositionPatient` é o centro do primeiro pixel; `ImageOrientationPatient`
  traz o vetor das linhas (coluna crescente) e das colunas (linha crescente);
  `PixelSpacing[0]` é a distância entre linhas e `[1]` entre colunas.
- Normal = linha × coluna. A ordenação é por `IPP · normal`; o sentido
  (cabeça→pés ou o inverso) segue a tendência do InstanceNumber, para que o
  scroll acompanhe a numeração do equipamento, mas a ordem é sempre espacial.
- Uma "série" DICOM pode conter várias pilhas: o sorter separa por classe de
  geometria (orientação, FoR, dimensões, espaçamento) e, quando há posições
  duplicadas, por eco, TE, posição temporal, b-value, trigger, aquisição,
  Stack ID e Image Type. Localizadores em 3 planos ficam como uma pilha
  não-volumétrica; clipes de US multiframe viram "Clipe N".
- `StackGeometry` registra: espacial/paralela/volumétrica, espaçamento
  mediano, lacunas (> 1,5 × o espaçamento), espaçamento irregular (tolerância
  2 %), gantry tilt (deslocamento no plano > 0,1 mm), duplicatas, FoR mistos
  e cor. Pilhas com duplicatas não explicadas não são usadas em MPR.
- O `ImageVolume` guarda a **origem de cada corte** (não assume grade
  regular): gantry tilt e espaçamento irregular são amostrados nas posições
  reais; lacunas ficam vazias (NaN, exibidas em preto), nunca interpoladas.
- MPR: as vistas ortogonais são alinhadas aos eixos do volume mais próximos
  dos eixos anatômicos (axial: u=+x, v=+y; coronal: u=+x, v=−z; sagital:
  u=+y, v=−z). O `Reslicer` usa formas lineares (sem matemática vetorial por
  pixel) e `parallelFor`; o slab amostra ao longo da normal com passo igual
  ao espaçamento do eixo alinhado (ou metade do menor espaçamento em planos
  oblíquos) — média, MIP ou MinIP.
- Sincronização: só entre imagens com o mesmo Frame of Reference UID não
  vazio e planos paralelos; escolhe o corte cuja posição projetada na
  normal é a mais próxima (não o mesmo índice). Reference lines: interseção
  do retângulo do plano de origem com o plano de destino.

### Concorrência

| Thread | Função |
|---|---|
| GUI | eventos, render 2D (LUT + QPainter), anotações |
| ImportTask | varredura e parse (2–4 workers em paralelo) |
| FrameProvider (N = núcleos − 1, entre 2 e 6; ajustável) | decodificação priorizada, cada uma com seu worker |
| Construção de volume MPR | `std::thread` com barra de progresso e cancelamento |
| `parallelFor` | reslice MPR e estatísticas pesadas |

Objetos DICOM são imutáveis depois de criados (`shared_ptr<const ...>`);
a UI só recebe resultados prontos via sinais em fila. Encerramento: a
`MainWindow` destrói a grade antes dos provedores (testado com ASan).

### Isolamento de processo (segurança e estabilidade)

Codecs de terceiros têm falhas de memória reais com arquivos malformados
(encontradas por fuzzing neste projeto: leitura fora dos limites no parser
J2K do GDCM, estouro de heap no caminho JPEG-LS, ponteiro nulo no codec
JPEG com precisão inválida, divisão por zero no RLE). Por isso:

- Todo parse/decodificação feito pela aplicação acontece no
  `visualtc-worker` (protocolo `[u64 tamanho][payload]`, operações Parse,
  Decode e Ping). Se o worker morre, o `DecoderClient` relata erro para
  aquele arquivo e reinicia o processo; o visualizador continua.
- Toda resposta do worker é validada (dimensões, contagem de bytes, totais
  dentro do limite) antes de virar objeto na aplicação.
- No worker: sem core dump (`RLIMIT_CORE=0`), limite de memória virtual
  de 12 GiB no Linux, requisições ≤ 1 MiB.
- Além do isolamento, o preflight e as verificações de cabeçalho JPEG/RLE
  impedem as falhas conhecidas mesmo no modo `--no-isolation` (13 500
  arquivos JPEG/JPEG Lossless/RLE corrompidos decodificados em processo sob
  ASan sem falhas).

### Privacidade

Nenhuma comunicação de rede (o Qt é usado sem o módulo Network). O log
local não registra nomes, IDs, datas ou UIDs; caminhos de arquivo só no
nível DEBUG. A exportação permite ocultar a identificação do paciente. O
arquivo original nunca é modificado (todas as leituras são `ifstream` em
modo binário somente leitura).

## B. Escolha das tecnologias

| Necessidade | Escolha | Por quê | Alternativa considerada |
|---|---|---|---|
| Linguagem | C++20 | desempenho, acesso direto a GDCM/VTK/ITK, `std::numbers`, `std::span`-like, conceitos simples | Rust (ecossistema DICOM/GUI imaturo) |
| Interface | Qt 6.8 LTS Widgets | nativo nos 3 SOs, HiDPI, multimonitor, maduro, LGPL | Qt Quick (menos adequado para ferramentas densas de estação de trabalho) |
| DICOM principal | **GDCM 3.0.24** | leitura tolerante de arquivos reais, todos os codecs necessários embutidos (IJG 8/12/16, OpenJPEG, CharLS, RLE), licença BSD, build estático simples | DCMTK: excelente para rede/PACS, mas JPEG 2000 não é livre (só via módulo comercial/externo) e o build com todos os codecs é mais pesado. Fica reservado para a fase PACS (C-FIND/C-MOVE/C-STORE). |
| 2D e MPR | pipeline próprio em CPU (double na geometria) | controle total da geometria (tilt, espaçamento irregular, lacunas) e testes exatos; leve, sem OpenGL | VTK `vtkImageReslice`: assume grade regular e exigiria reamostrar antes, escondendo lacunas |
| 3D (v1.0) | VTK 9.3 (`vtkSmartVolumeMapper`, `QVTKOpenGLNativeWidget`) | ray casting GPU com fallback CPU, presets, clipping/cropping | implementação própria (não compensa) |
| Processamento avançado (fase 3+) | ITK 5.4, se necessário | filtros/segmentação; não é dependência do MVP | — |
| Build | CMake ≥ 3.21 + presets + vcpkg (baseline fixa) | reprodutível nos 3 SOs e no CI | Conan (equivalente; vcpkg tem GDCM com codecs prontos) |
| Testes | Catch2 3.7.1 (núcleo) + QtTest (UI offscreen) | rápidos, sem dependências extras | GoogleTest |

## C. Estrutura de diretórios

```
VisualTC/
├── CMakeLists.txt, CMakePresets.json, vcpkg.json
├── cmake/             CompilerSettings (avisos, sanitizers), Packaging (install/CPack)
├── src/
│   ├── core/          utilitários sem Qt (log, threads, vetores, caminhos UTF-8)
│   ├── dicom/         preflight, parser, scanner, sorter, geometria, decoder, protocolo do worker
│   ├── imaging/       DecodedFrame, VOI/LUT, orientação, cache LRU
│   ├── measurements/  matemática de medidas e estatísticas de ROI
│   ├── mpr/           volume, reslicer, geometria MPR (+ MprSession/MprSource na UI)
│   ├── synchronization/ sincronização espacial e reference lines
│   ├── worker/        visualtc-worker
│   ├── io/            DecoderClient, FrameProvider, ImportTask, miniaturas
│   ├── viewer2d/      Viewport, fontes de imagem, anotações
│   ├── ui/            janela principal e diálogos
│   ├── export/        PNG/JPEG/TIFF
│   └── app/           main, tema, preferências
├── tests/  unit/ (Catch2), ui/ (QtTest), fixtures/ (gerador DICOM sintético)
├── tools/  make_phantom (exame sintético), vtc_bench (desempenho)
├── resources/icons/   ícones SVG originais
├── packaging/         linux (.desktop), macos (Info.plist, .icns), windows (.rc, .ico, Inno Setup)
├── scripts/           build_deps_linux.sh
├── docs/              esta documentação + screenshots
└── .github/workflows/ CI Windows/macOS/Ubuntu + ASan/UBSan + clang-tidy
```

## D. Dependências e versões fixadas

| Dependência | Versão | Licença | Uso |
|---|---|---|---|
| Qt (qtbase, qtsvg, qtimageformats) | 6.8.3 LTS | LGPL-3.0 (link dinâmico) | interface |
| GDCM | 3.0.24 | BSD-3-Clause | leitura DICOM e codecs |
| ↳ OpenJPEG | 2.3 (embutido no GDCM) / 2.5 (vcpkg) | BSD-2-Clause | JPEG 2000 |
| ↳ CharLS (embutido no GDCM) | 2.0 | BSD-3-Clause | JPEG-LS |
| ↳ IJG libjpeg 6b modificada (embutida) | — | IJG | JPEG 8/12/16 bits |
| ↳ zlib, expat (embutidos) | — | zlib / MIT | Deflate, dicionários |
| Catch2 | 3.7.1 | BSL-1.0 | somente testes |
| VTK (planejado, fase 5) | 9.3 | BSD-3-Clause | volume rendering |
| DCMTK (planejado, fase PACS) | 3.6.8 | BSD-3-Clause | rede DICOM |

O `vcpkg.json` fixa `builtin-baseline` (vcpkg 2026.07.29) e `overrides` para
GDCM 3.0.24 e Catch2 3.7.1; o Qt é fixado em 6.8.3 no CI.

## E. Roadmap

| Fase | Conteúdo | Situação |
|---|---|---|
| 1 — Fundação | CMake/presets/vcpkg, CI, scanner, parser, sorter, geometria, decoder, cache, worker isolado, testes do núcleo | **concluída** |
| 2 — Visualizador 2D (MVP) | janela, séries, miniaturas, viewport, scroll, W/L, zoom, pan, overlays, orientação, presets, rotação/espelho/inversão, cine | **concluída** |
| 3 — Medidas e multiview | régua, ângulo, Cobb, ROIs com HU, sonda, histograma, undo/redo, layouts, sincronização, reference lines, exportação | **concluída** |
| 4 — MPR | axial/coronal/sagital, crosshair, oblíquo, thick slab MIP/MinIP/média | **concluída** (oblíquo por passos de 5°; rotação livre por arrasto a fazer) |
| 5 — 3D | VTK volume rendering, presets, clipping, cropping, MIP 3D | a fazer |
| 6 — Avançado | CPR, curvas, fusão, comparação de exames, DICOM SEG/RTSTRUCT, PACS (DCMTK) | a fazer |
| Contínuo | empacotamento assinado (Authenticode, Developer ID + notarização), traduções, acessibilidade | parcialmente (pacotes sem assinatura no CI) |

## F. Critérios de aceite do MVP (marco da seção 99)

| Critério | Como é verificado |
|---|---|
| Abre nos 3 SOs | CI compila e testa Windows, macOS ARM, macOS Intel e Ubuntu; testado localmente no Linux |
| Escolher pasta e encontrar DICOM sem extensão, em subpastas | teste `Scanner finds DICOM files without extension in nested folders` |
| Organizar estudos/séries | `Database groups patients, studies and series and drops duplicates` |
| Miniaturas | ThumbnailProvider; verificado em screenshots |
| Abrir série e percorrer cortes na ordem anatômica | testes de ordenação + UI `wheelAndKeyboardNavigation` |
| W/L, zoom, pan | UI `windowLevelDragFollowsMouse`, `displaysImageWithDicomWindow` |
| HU corretos | `QA de HU: stored 1000, slope 1, intercept -1024 = -24 HU`, `roiReportsHounsfieldUnits` |
| Distância correta | `QA de medidas: 100 px x 0.5 mm = 50 mm`, `distanceMeasurementInMillimetres` |
| Sem PixelSpacing → sem mm inventado | `Missing PixelSpacing is never invented` |
| Arquivo corrompido não derruba | fuzzing + `isolatedDecoderSurvivesCrash` |
| Desempenho | `vtc_bench`: varredura de 425 arquivos em 20 ms; decodificação 0,5–5 ms/imagem; W/L < 0,5 ms; MPR 2–4 ms por plano |

## G. Riscos técnicos e mitigação

| Risco | Impacto | Mitigação adotada |
|---|---|---|
| Falhas de memória em codecs de terceiros com arquivos hostis | travamento, execução de código | processo isolado + validação do protocolo + preflight + verificações JPEG/RLE + fuzzing sob ASan |
| Arquivos "reais" fora do padrão (charset, VR errada, PI incoerente, YBR em JPEG declarado RGB) | imagem errada | GDCM tolerante, regras específicas testadas (ex.: espaço de cor lido do próprio fluxo JPEG) |
| Ordenação incorreta (InstanceNumber enganoso, tilt, ecos misturados) | erro diagnóstico | ordenação por geometria, divisão de pilhas, avisos visíveis, testes dedicados |
| Medidas sem calibração | erro diagnóstico | só mm com PixelSpacing/ImagerPixelSpacing/US region; aviso "SEM CALIBRAÇÃO" / "CALIBRAÇÃO NO DETECTOR" |
| Volumes grandes (> 2 GB) em máquinas modestas | falta de memória | int16 + slope/intercept no volume, limite configurável, cache LRU por bytes, mensagens claras |
| Assinatura/notarização no macOS e Windows | alertas do SO ao instalar | documentado em BUILD.md; o CI produz pacotes não assinados até haver certificados |
| Qt LGPL | obrigações de distribuição | link dinâmico, aviso em THIRD_PARTY_LICENSES.md |
| Uso clínico sem certificação | regulatório | aviso no "Sobre" e no README; o software não é dispositivo médico registrado |
