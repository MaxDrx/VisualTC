# Guia do usuário — VisualTC 0.1.0

> O VisualTC 0.1.0 não é um dispositivo médico registrado. Confira medidas e
> reconstruções antes de qualquer uso diagnóstico.

## 1. Abrir um exame

- **Pasta** (Ctrl+Shift+O): escolha a pasta do CD/pendrive ou a pasta
  baixada do PACS. Todas as subpastas são lidas; não importa se os arquivos
  têm extensão `.dcm` ou nenhuma.
- **Abrir** (Ctrl+O): um ou mais arquivos.
- **Arrastar e soltar** pastas ou arquivos na janela.
- Pela linha de comando: `VisualTC /caminho/do/exame`.

A barra de status mostra o progresso. Ao final, a maior série (que não seja
topograma) abre automaticamente. Arquivos com problema não interrompem a
importação: o resumo informa quantos foram e o menu **Arquivo › Problemas de
importação** lista o motivo de cada um (corrompido, truncado, formato não
suportado…). Os arquivos originais nunca são alterados.

## 2. Painel de estudos e séries

À esquerda ficam paciente, estudo (data e descrição) e séries com miniatura,
modalidade, número de imagens, espessura e a marca **MPR** quando a série é
volumétrica. Avisos aparecem na própria série (ex.: "cortes ausentes",
"gantry tilt", "espaçamento irregular").

- **Duplo clique** abre a série no viewport ativo.
- **Arraste** a série para qualquer viewport.
- **F2** mostra/oculta o painel.
- O menu de contexto (botão direito) abre a série no viewport ativo,
  direto em MPR, ou mostra as informações DICOM.

Uma série DICOM com várias pilhas (ecos, fases, clipes de US, planos
diferentes) aparece dividida, com o motivo no nome (ex.: "Eco 2", "Clipe 3").

## 3. Navegação

| Ação | Como |
|---|---|
| Próximo/anterior corte | roda do mouse, setas ↑ ↓ ← →, ferramenta **Navegar cortes** (S) e arrastar |
| Pular 10 % da série | Page Up / Page Down |
| Primeiro/último | Home / End |
| Cine | **Espaço**; velocidade, loop e sentido reverso no menu Imagem |
| Zoom | Ctrl+roda, botão direito arrastando, ferramenta **Zoom** (Z) |
| Pan | botão do meio, Shift+arrastar, ferramenta **Pan** (P) |
| Ajustar à janela / 1:1 | F / Shift+F |
| Reset da imagem | Ctrl+0 |
| Próximo viewport | Tab |
| Maximizar viewport | duplo clique (de novo para voltar) |
| Tela cheia | F11 (ou o atalho de tela cheia do sistema) |

O scroll sempre segue a ordem **anatômica** (posição de cada corte no
espaço), no mesmo sentido da numeração do equipamento. A posição em mm e
"Imagem X / N" ficam no canto inferior direito.

## 4. Window/Level e presets

- Ferramenta **Window/Level** (W) com o botão esquerdo: arrastar na
  horizontal muda a largura (WW), na vertical muda o centro (WL).
- Teclas **1–8** aplicam os presets de TC: 1 Pulmão, 2 Mediastino,
  3 Abdome, 4 Fígado, 5 Osso, 6 Cérebro, 7 Subdural, 8 AVC.
- Menu **Imagem › Presets de janela** traz também as janelas gravadas no
  arquivo DICOM e os presets personalizados (Preferências › DICOM).
- **VOI LUT do arquivo** usa a tabela VOI gravada pelo equipamento, quando
  existe.
- **Inverter** (I). Imagens MONOCHROME1 (algumas radiografias) já são
  exibidas corretamente; a inversão do usuário é aplicada por cima.

Os valores WW/WL exibidos estão em HU na TC.

## 5. Orientação e transformações

As letras nas bordas (R/L, A/P, H/F) são calculadas a partir da orientação
gravada no arquivo; em planos oblíquos aparecem combinações como "RA". Sem
orientação no arquivo, nenhuma letra é mostrada.

Girar 90° (] e [), rotação livre (menu Imagem), espelhar (H e Shift+H).
Enquanto a imagem estiver girada, espelhada ou invertida, um aviso
aparece no viewport e as letras de orientação acompanham a transformação.

## 6. Medidas e ROIs

| Ferramenta | Tecla | Uso |
|---|---|---|
| Régua | M | arraste do ponto inicial ao final; resultado em mm (ou cm) |
| Ângulo | A | arraste o primeiro segmento e clique no terceiro ponto |
| Cobb | C | arraste a primeira linha e depois a segunda |
| ROI retangular | R | arraste |
| ROI elíptica | E | arraste |
| ROI livre | L | desenhe segurando o botão |
| Valor do pixel | V | clique ou arraste: posição, valor e HU |

- As ROIs mostram área, média, desvio-padrão, mínimo e máximo (em **HU** na
  TC). Pixels fora da imagem são ignorados.
- **Histograma da ROI** (Ctrl+H) para a ROI selecionada.
- Clique numa medida para selecioná-la; arraste os pontos para editar, o
  corpo para mover e o texto para reposicioná-lo.
- **Delete/Backspace** apaga a selecionada; Ctrl+Shift+Del apaga todas;
  Ctrl+Z / Ctrl+Shift+Z (Ctrl+Y no Windows) desfazem e refazem.
- **Esc** cancela uma medida em andamento.
- As medidas ficam associadas à imagem em que foram feitas.

**Calibração.** As distâncias usam o PixelSpacing do arquivo. Se o arquivo
só tem o espaçamento no detector (algumas radiografias), aparece
**CALIBRAÇÃO NO DETECTOR** (não corrige a magnificação). Sem nenhuma
calibração, as medidas são mostradas em **pixels** e o aviso
**SEM CALIBRAÇÃO** aparece — o VisualTC nunca inventa um valor em mm.

## 7. Vários viewports, sincronização e linhas de referência

- **Layout** (barra de ferramentas ou Ctrl+1…Ctrl+6): 1×1, 1×2, 2×1, 2×2,
  3×2, 3×3. Cada viewport guarda a sua série e o seu estado.
- **Sincronizar séries** (Y): ao navegar uma série, as outras do mesmo
  sistema de coordenadas (Frame of Reference) vão para o corte mais próximo
  **anatomicamente** — não para o mesmo número de imagem. Séries de outro
  exame ou sem coordenadas não são sincronizadas (o viewport indica isso).
  Zoom/pan e window/level podem ser sincronizados também (Preferências).
- **Linhas de referência** (Ctrl+L): a posição do corte ativo aparece como
  linha nas outras séries, inclusive no topograma.

## 8. MPR

- **MPR** (Ctrl+M ou botão na barra) monta o volume da série ativa e mostra
  axial, coronal e sagital lado a lado. Disponível para séries marcadas
  como **MPR** no painel (cortes paralelos e coerentes).
- **Cruz** (X): clique ou arraste em um plano para posicionar os outros
  dois. A roda do mouse percorre cada plano de forma independente.
- **Thick slab** (menu MPR › Espessura): 1–50 mm ou valor personalizado até
  500 mm, nos modos **MIP**, **MinIP** e **Média**.
- **Planos oblíquos**: Ctrl+[ e Ctrl+] giram os outros dois planos em
  passos de 5° em torno do eixo do plano ativo; "Restaurar planos
  ortogonais" desfaz.
- Gantry tilt e espaçamento irregular são reconstruídos nas posições reais
  dos cortes; regiões de cortes ausentes ficam pretas (nunca interpoladas).
- Clique em **MPR** novamente para voltar à visualização 2D.

## 9. Exportar e capturar

- **Exportar imagem** (Ctrl+E): PNG, JPEG ou TIFF do viewport ativo, com
  opções de incluir medidas/textos e de **ocultar a identificação do
  paciente**.
- **Capturar viewport** (Ctrl+Shift+C): copia a imagem para a área de
  transferência.
- **Informações DICOM** (Ctrl+I): principais atributos da imagem atual.

## 10. Preferências

| Aba | Opções |
|---|---|
| Interface | tema escuro/claro, tamanho da fonte |
| Mouse | ação dos botões esquerdo, do meio e direito |
| Desempenho | tamanho do cache, pré-carregamento da série, threads, interpolação, decodificação isolada, qualidade do MPR |
| DICOM | textos sobre a imagem, linhas de referência, sincronização de zoom/pan e janela, presets personalizados |

## 11. Atalhos

| Tecla | Função | Tecla | Função |
|---|---|---|---|
| Ctrl+O | Abrir arquivos | Ctrl+Shift+O | Abrir pasta |
| F2 | Painel de estudos | Ctrl+1…6 | Layouts |
| W | Window/Level | P | Pan |
| Z | Zoom | S | Navegar cortes |
| M | Régua | A | Ângulo |
| C | Cobb | R | ROI retangular |
| E | ROI elíptica | L | ROI livre |
| V | Valor do pixel | X | Cruz do MPR |
| I | Inverter | F / Shift+F | Ajustar / 1:1 |
| ] / [ | Girar 90° | H / Shift+H | Espelhar |
| Espaço | Cine | O | Textos sobre a imagem |
| Y | Sincronizar | Ctrl+L | Linhas de referência |
| Ctrl+M | MPR | Ctrl+[ / Ctrl+] | Oblíquo ±5° |
| 1–8 | Presets de TC | Ctrl+0 | Reset |
| Ctrl+E | Exportar | Ctrl+Shift+C | Capturar |
| Ctrl+H | Histograma | Ctrl+I | Informações DICOM |
| Ctrl+Z | Desfazer | Delete | Apagar medida |
| Esc | Cancelar / ferramenta padrão | Tab | Próximo viewport |

A lista também está em **Ajuda › Atalhos de teclado**.

## 12. Privacidade e log

O VisualTC não acessa a internet. O log técnico (`visualtc.log`, local
indicado em **Ajuda › Local do arquivo de log**) não contém nomes, IDs,
datas nem identificadores do exame.
