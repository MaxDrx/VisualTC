# Estado do projeto — VisualTC 0.1.0

Atualizado em 05/10/2026. Convenção: um item só entra em **IMPLEMENTADO**
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
- CMake + presets + vcpkg com versões fixadas; CI Windows/macOS ARM/macOS
  Intel/Ubuntu; empacotamento AppImage, .deb, instalador Inno Setup e DMG
  (sem assinatura); documentação completa em `docs/`.

## EM DESENVOLVIMENTO
- **Validação nos runners do CI**: os pacotes de Windows e macOS e o
  AppImage estão configurados no workflow, mas não foram executados neste
  ambiente (ver "Testes realizados").
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
- Assinatura de código (Authenticode, Developer ID + notarização) e
  binário universal macOS.
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
GDCM 3.0.24, Catch2 3.7.1.

| Conjunto | Resultado |
|---|---|
| `ctest` Release (`linux-local`) | 77/77 aprovados (76 casos Catch2 + suíte de interface) |
| `ctest` Debug + ASan + UBSan | 77/77 aprovados, sem erros do sanitizer |
| Interface (QtTest offscreen) | 8 testes: janela DICOM, arrasto W/L, roda/teclado, régua 50 mm, ROI em HU, sincronização espacial, crosshair do MPR, queda do worker |
| QA numérico | 1000 × 1 − 1024 = −24 HU; 100 px × 0,5 mm = 50 mm; VOI conforme PS3.3; MPR exato em fantomas lineares (axial, tilt, espaçamento irregular, oblíquo) |
| Fuzzing (ASan, em processo) | 3 000 arquivos nativos corrompidos + 13 500 JPEG/JPEG Lossless/RLE corrompidos: nenhuma falha |
| Isolamento | queda forçada do worker durante a decodificação: app continua e reinicia o worker |
| Verificação visual | capturas em `docs/screenshots/` (1×1, 2×2 com sincronização e medidas, 3×3 com todas as modalidades, MPR fino e MIP 20 mm) |
| Desempenho (`vtc_bench`, Release, 2 núcleos) | varredura de 425 arquivos: 21 ms; decodificação por imagem: nativo 0,5 ms, JPEG-LS 1,0 ms, J2K 4,5 ms, RLE 4,7 ms, US 40 quadros RGB 17 ms; W/L 0,1–0,9 ms; volume 400×400×221 em 234 ms; MPR plano fino 2–3 ms; MIP 20 mm 44–64 ms |
| Empacotamento | `.deb` montado localmente a partir da árvore de instalação (script do CI); AppImage, instalador Windows e DMG **não executados aqui** |
| Windows / macOS | **não compilados neste ambiente**; configurados no CI (GitHub Actions) |
