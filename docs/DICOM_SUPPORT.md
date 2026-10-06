# Suporte DICOM do VisualTC 0.3.0

Este documento descreve o que o VisualTC lê, como interpreta cada atributo
relevante e quais são as limitações conhecidas. "Testado" significa coberto
por teste automatizado com arquivos gerados pelos fixtures (ver
`tests/unit/test_dicom_io.cpp`); "suportado" significa tratado pelo mesmo
caminho de código/codec, sem arquivo de teste dedicado.

O VisualTC é um **visualizador**: nunca grava, renomeia ou altera os arquivos
originais.

## 1. Detecção e leitura de arquivos

| Situação | Comportamento |
|---|---|
| Arquivo Part 10 (preâmbulo de 128 bytes + `DICM`) | lido normalmente — **testado** |
| Dataset "cru" sem preâmbulo e sem File Meta (equipamentos antigos) | reconhecido pelo conteúdo, Implicit VR LE — **testado** |
| Arquivos sem extensão, com extensão qualquer, em subpastas | encontrados pela varredura — **testado** |
| DICOMDIR | ignorado como índice; as imagens são encontradas pela varredura da pasta |
| Arquivos não DICOM na pasta | ignorados silenciosamente |
| Objetos sem pixels (SR, PR, KO, RT Plan, PDF encapsulado…) | listados no relatório de importação como "Objeto DICOM sem imagem" |
| Arquivo truncado (pixels incompletos) | relatado como "Arquivo truncado" e não incluído na série (aparece como corte ausente) — **testado** |
| Estrutura corrompida (comprimentos impossíveis, VR inválida, fragmentos corrompidos, cabeçalho RLE inválido) | rejeitado pelo preflight antes de qualquer alocação — **testado** (fuzzing) |
| Mesmo SOP Instance UID em dois arquivos | contado uma única vez — **testado** |
| Caminhos com acentos/Unicode | suportados nos 3 SOs (leitura por `std::filesystem::path`) — **testado** |

Limites padrão (protegem contra arquivos hostis e "bombas" de
descompressão): 32 768 linhas/colunas, 20 000 quadros, 4 GiB de pixels
decodificados por arquivo, 8 GiB por arquivo, 500 000 arquivos por
varredura. Arquivos acima dos limites são relatados, nunca abertos
parcialmente.

### Exames compactados

| Formato | Situação |
|---|---|
| ZIP | **testado** (Deflate e sem compressão); Deflate64, BZip2, LZMA, XZ e Zstd suportados |
| ZIP com senha | **testado** (ZipCrypto e AES-256; AES-128/192 suportados); pede a senha, até 3 tentativas |
| 7z | **testado** |
| RAR 4 e RAR 5 | suportado (leitura pela libarchive; sem gerador para teste automatizado) |
| TAR, TAR.GZ/TGZ, TAR.BZ2, TAR.XZ, TAR.ZST | **testado** |
| Um único arquivo comprimido (`.dcm.gz`; `.bz2`, `.xz`, `.zst`) | **testado** (gzip); demais suportados |
| Imagem de CD/DVD ISO 9660 (com Joliet/Rock Ridge) | **testado** |
| Arquivo compactado dentro de outro (até 3 níveis) | **testado** |
| 7z e RAR **com senha** | não suportado: mensagem pedindo para descompactar antes |
| Volumes divididos (`.z01`, `.part1.rar`, `.7z.001`) | não suportado |

O tipo é detectado pelo **conteúdo** (assinatura), não pela extensão. A
extração roda no processo isolado (`visualtc-worker`) e grava só os membros
que parecem DICOM (ou outros arquivos compactados), com nomes numerados numa
pasta temporária da sessão: os nomes internos nunca viram caminhos, então
`../`, caminhos absolutos, links simbólicos e dispositivos não têm efeito —
**testado**. Limites: 64 GiB extraídos por exame, 8 GiB por membro, 500 000
membros, 1 GiB livre em disco sempre preservado e taxa de compressão máxima
de 200× após o primeiro GiB ("bombas" de compressão são recusadas) —
**testado**. Arquivos compactados danificados são relatados sem derrubar o
programa — **testado** (fuzzing). A pasta temporária é apagada ao fechar os
estudos ou o programa, e a de uma sessão interrompida é apagada na próxima
abertura.

## 2. Transfer Syntaxes

| UID | Nome | Situação |
|---|---|---|
| 1.2.840.10008.1.2 | Implicit VR Little Endian | **testado** |
| 1.2.840.10008.1.2.1 | Explicit VR Little Endian | **testado** |
| 1.2.840.10008.1.2.1.99 | Deflated Explicit VR Little Endian | **testado** |
| 1.2.840.10008.1.2.2 | Explicit VR Big Endian (aposentada) | **testado** |
| 1.2.840.10008.1.2.4.50 | JPEG Baseline (Process 1) | **testado** (inclusive cor) |
| 1.2.840.10008.1.2.4.51 | JPEG Extended (Process 2 & 4, 12 bits) | suportado (codec IJG 12 bits) |
| 1.2.840.10008.1.2.4.57 | JPEG Lossless (Process 14) | suportado |
| 1.2.840.10008.1.2.4.70 | JPEG Lossless SV1 | **testado** |
| 1.2.840.10008.1.2.4.80 | JPEG-LS Lossless | **testado** |
| 1.2.840.10008.1.2.4.81 | JPEG-LS Near-Lossless | suportado (mesmo codec) |
| 1.2.840.10008.1.2.4.90 | JPEG 2000 Lossless | **testado** |
| 1.2.840.10008.1.2.4.91 | JPEG 2000 | suportado (mesmo codec) |
| 1.2.840.10008.1.2.5 | RLE Lossless | **testado** |
| 1.2.840.10008.1.2.4.92 / .93 | JPEG 2000 Part 2 multicomponente | **não suportado** — mensagem clara |
| 1.2.840.10008.1.2.4.100–.108 | MPEG-2, H.264, H.265 (vídeo) | **não suportado** — mensagem clara |
| 1.2.840.10008.1.2.4.201–.203 | High-Throughput JPEG 2000 | **não suportado** — mensagem clara |
| 1.2.840.10008.1.2.4.110+ | JPEG XL | **não suportado** — mensagem clara |

Imagens com compressão com perdas (pela Transfer Syntax ou por
`LossyImageCompression = 01`) exibem o aviso **COMPRESSÃO COM PERDAS**.

## 3. Formato de pixel

| Atributo | Suporte |
|---|---|
| Bits Allocated | 1, 8, 16, 32 (inteiros) — 1 bit é desempacotado; Float/Double Float Pixel Data (mapas paramétricos) ainda não: o arquivo é relatado como "ponto flutuante não suportado" |
| Bits Stored / High Bit | bits fora da faixa armazenada são mascarados e o sinal é estendido corretamente (ex.: 12 bits com "lixo" nos bits altos) — **testado** |
| Pixel Representation | sem sinal e complemento de dois — **testado** |
| Samples per Pixel | 1 e 3 |
| Planar Configuration | 0 e 1 |
| Multiframe | sim, inclusive clipes de US e Enhanced CT/MR |

### Photometric Interpretation

| Valor | Tratamento |
|---|---|
| MONOCHROME2 | direto — **testado** |
| MONOCHROME1 | invertido na exibição (combinado por XOR com a inversão do usuário) — **testado** |
| RGB | direto — **testado** (todos os codecs) |
| YBR_FULL | convertido para RGB (BT.601, faixa completa) — **testado** |
| YBR_FULL_422 nativo | GDCM faz o up-sampling; convertido para RGB — **testado** |
| YBR_FULL_422 em JPEG | o espaço de cor é lido do **próprio fluxo JPEG** (marcador JFIF, Adobe APP14 ou IDs dos componentes, como faz a libjpeg); arquivos que declaram RGB mas contêm YCbCr (comum em US e captura secundária) também saem com as cores corretas — **testado** |
| YBR_PARTIAL_422 | convertido com a faixa reduzida (16–235) |
| YBR_ICT / YBR_RCT (JPEG 2000) | a transformada inversa é feita pelo OpenJPEG; resultado RGB |
| PALETTE COLOR | aplicado com as LUTs vermelha/verde/azul (8 ou 16 bits) — **testado** |
| YBR_PARTIAL_420, HSV, ARGB, CMYK | não suportados (aposentados ou exclusivos de vídeo) |

## 4. Valores de pixel e janelamento

- **Modality LUT**: `RescaleSlope`/`RescaleIntercept` por quadro (inclusive
  Pixel Value Transformation em Enhanced). HU = valor × slope + intercept,
  calculado em double — teste de QA: 1000 × 1 − 1024 = −24 HU.
- **VOI**: `WindowCenter`/`WindowWidth` (todos os pares, com
  `WindowCenterWidthExplanation`), funções `LINEAR`, `LINEAR_EXACT` e
  `SIGMOID` exatamente como PS3.3 C.11.2.1.2 — **testado**; VOI LUT tabelada
  (menu "VOI LUT do arquivo") — **testado**.
- Sem janela no arquivo: janela automática pelos percentis 0,5–99,5 % (não
  pelo mínimo/máximo, que costuma ser dominado pelo ar ou por metal).
- Presets de TC: Pulmão (−600/1500), Mediastino (40/400), Abdome (50/400),
  Fígado (30/150), Osso (400/1800), Cérebro (40/80), Subdural (75/200),
  AVC (40/40), mais presets personalizados nas Preferências.
- ROIs e sonda de pixel mostram valores em **HU** para TC e no valor de
  modalidade (sem unidade) para as demais; imagens coloridas mostram
  geometria, mas não estatísticas.

## 5. Geometria e calibração

| Fonte | Uso |
|---|---|
| `PixelSpacing (0028,0030)` | medidas em mm — `[0]` = entre linhas, `[1]` = entre colunas (espaçamento anisotrópico respeitado) |
| `ImagerPixelSpacing (0018,1164)` (RX/MG sem PixelSpacing) | mm no plano do detector, com aviso **CALIBRAÇÃO NO DETECTOR** |
| Pixel Measures (Enhanced, functional groups) | por quadro |
| `SequenceOfUltrasoundRegions` (US) | mm a partir de `PhysicalDeltaX/Y` em cm, da primeira região 2D (`RegionSpatialFormat` = 1) com unidades em cm; regiões de M-mode, Doppler espectral e traçados são ignoradas |
| nenhuma das anteriores | medidas em **pixels**, aviso **SEM CALIBRAÇÃO**; nenhum valor em mm é inventado — **testado** |

- `ImagePositionPatient`, `ImageOrientationPatient`, `FrameOfReferenceUID`
  (inclusive Plane Position/Orientation por quadro em Enhanced) definem a
  posição 3D de cada pixel. Vetores de orientação arredondados (desvio de
  ortogonalidade < 0,01, ~0,6°) são ortonormalizados mantendo a direção das
  linhas; vetores mais distorcidos não são usados (sem orientação, sem MPR).
- Valores decimais (DS) são lidos sempre com ponto decimal, independentemente
  do idioma/localidade do sistema operacional.
- Cortes de uma mesma série com espaçamento de pixel diferente (outro FOV)
  formam pilhas separadas e nunca são intercalados num volume.
- Letras de orientação (R/L, A/P, H/F) calculadas dos vetores de orientação,
  com até três componentes em planos oblíquos (ex.: "RA", "HPL"); sem
  orientação, nenhuma letra é exibida.
- A espessura exibida é `SliceThickness`; o espaçamento usado em MPR e
  sincronização é o real, medido entre as posições.
- Avisos de pilha: lacunas, espaçamento irregular, gantry tilt, posições
  duplicadas, Frames of Reference diferentes.

## 6. Modalidades e objetos

| Modalidade | Situação |
|---|---|
| CT (inclusive Enhanced CT) | prioridade; HU, presets, MPR, slab — **testado** |
| MR (inclusive Enhanced MR, multi-eco, difusão, cardíaco) | separação por eco/TE/b-value/fase — **testado** (eco) |
| CR/DX/MG | MONOCHROME1, ImagerPixelSpacing — **testado** (fantoma) |
| US (cine, multiframe colorido) | calibração por região, clipes separados, cine — **testado** (fantoma) |
| XA/RF, NM, PT, OT, SC | exibidos pelo caminho genérico (sem SUV para PET nesta versão) |
| SR, PR, KO, SEG, RTSTRUCT, RT Dose | não exibidos nesta versão |

## 7. Conjuntos de caracteres

| Specific Character Set | Situação |
|---|---|
| vazio (ASCII) | **testado** |
| ISO_IR 100 (Latin-1, padrão no Brasil) | **testado** |
| ISO_IR 192 (UTF-8) | **testado** (fantoma de RM) |
| UTF-8 não declarado (não conforme, mas comum) | detectado e exibido corretamente |
| Demais (ISO 2022 japonês/coreano, GB18030, Latin-2, cirílico, grego, árabe, hebraico, tailandês) | **não convertidos nesta versão** — tratados como Latin-1; os nomes podem aparecer com caracteres errados (a imagem não é afetada) |

## 8. Mensagens de erro ao usuário

As mensagens são em português, sem dados do paciente e indicam o motivo:
"Este arquivo não pôde ser interpretado como DICOM", "Arquivo DICOM
corrompido (comprimento de elemento maior que o arquivo)", "Arquivo
truncado: os dados de pixel estão incompletos", "Transfer Syntax não
suportada: HEVC/H.265 Main Profile (…)", "Formato de pixel não suportado
(BitsAllocated=…)", "Imagem descomprimida excederia o limite de memória
configurado", "Fluxo JPEG corrompido (…)". Os arquivos com problema aparecem
no relatório de importação; a série continua utilizável com as imagens
válidas e mostra o aviso correspondente.
