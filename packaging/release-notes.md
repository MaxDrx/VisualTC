## Baixar e instalar o VisualTC @VERSION@

| Computador | Baixe este arquivo | Como instalar |
|---|---|---|
| **Windows 10 ou 11** | [**VisualTC-Setup-x64.exe**](https://github.com/@REPO@/releases/download/v@VERSION@/VisualTC-Setup-x64.exe) | Clique duas vezes, depois **Avançar** e **Concluir**. Não pede senha de administrador. |
| **Mac** — chip Apple (M1, M2, M3, M4…) **ou Intel**, macOS 12+ | [**VisualTC-macOS.dmg**](https://github.com/@REPO@/releases/download/v@VERSION@/VisualTC-macOS.dmg) | Clique duas vezes e arraste o VisualTC para a pasta **Aplicativos**. |
| **Ubuntu, Debian, Linux Mint** | [**visualtc_amd64.deb**](https://github.com/@REPO@/releases/download/v@VERSION@/visualtc_amd64.deb) | Clique duas vezes; na Central de Programas, clique em **Instalar**. |
| **Outras distribuições Linux** | [**VisualTC-x86_64.AppImage**](https://github.com/@REPO@/releases/download/v@VERSION@/VisualTC-x86_64.AppImage) | Botão direito › Propriedades › **Permitir executar como programa**; depois, clique duas vezes. |

Se o clique duplo no `.deb` não oferecer **Instalar** (algumas versões do Ubuntu), abra o Terminal na pasta
Downloads e digite `sudo apt install ./visualtc_amd64.deb`.

Os demais arquivos abaixo (código-fonte, `SHA256SUMS.txt`) não são necessários para usar o programa.

### Novidades desta versão

- **Reconstrução 3D** (botão **3D** na barra ou **Ctrl+D**), como nas estações de trabalho:
  - modos **Volume (VR)**, **Superfície**, **MIP**, **MinIP** e **Raio-X simulado**;
  - predefinições com miniatura do próprio exame — ossos, vasos (angio), partes moles e ossos, músculos, pele,
    pulmões, vias aéreas e as de ressonância — e as suas, salvas para usar em outros exames;
  - editor de cores e opacidade sobre o histograma, e **Ctrl + arrastar** na imagem para ajustar como
    janela/nível;
  - **caixa de recorte**, **tesoura 3D** (remover dentro ou fora do contorno, desfazer), vistas anterior,
    posterior, laterais, superior e inferior, cubo de orientação, perspectiva e **giro de 360°**;
  - salvar a imagem (PNG/JPEG), copiar e exportar o giro como sequência de imagens.
- **Funciona em qualquer computador**: usa a **placa de vídeo** (OpenGL 3.3) quando ela existe e, nos
  computadores antigos ou sem placa adequada, o **processador**, com imagem mais leve enquanto o volume gira.
  A escolha é automática (pode ser trocada em **Desempenho**), e se a placa falhar o programa passa sozinho
  para o processador.

### Apareceu um aviso na primeira vez?

- **Windows** — “O Windows protegeu o computador”: clique em **Mais informações** › **Executar assim mesmo**.
- **Mac** — “não é possível verificar o desenvolvedor”: abra **Ajustes do Sistema › Privacidade e Segurança** e clique em **Abrir Mesmo Assim** (só na primeira vez).

### Para abrir um exame

Arraste para a janela do VisualTC a pasta do CD/pendrive ou o arquivo compactado recebido (ZIP, RAR, 7z) —
ou use os botões **Abrir** e **Pasta**. No Windows, também dá para clicar com o botão direito no CD, numa pasta
ou num ZIP e escolher **Abrir no VisualTC**.

O VisualTC funciona sem internet: as imagens e os dados dos pacientes não saem do computador.
