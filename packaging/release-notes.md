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

- **MPR direto com o mouse**: arraste a linha colorida para mover o plano, a bolinha na ponta para girar
  (MPR oblíquo em qualquer ângulo) e a barrinha ao lado para dar espessura (MIP/MinIP) só àquele plano.
- **Linhas de referência** corrigidas: aparecem sobre o topograma mesmo quando ele tem outro sistema de
  coordenadas; com uma só imagem na tela, o botão **Ref.** abre o topograma ao lado.
- **Tabela de cores (LUT)**: ferro quente, PET, arco-íris, osso, cobre, fogo e gelo (botão **LUT**).
- **Histograma da ROI** corrigido (atalho ⌘⇧H no Mac, Ctrl+Shift+H no Windows/Linux).
- Barra de ferramentas com mais espaço entre os botões e adaptada a telas de notebook.
- A reconstrução 3D foi retirada para deixar o programa mais leve; o MPR continua completo.

### Apareceu um aviso na primeira vez?

- **Windows** — “O Windows protegeu o computador”: clique em **Mais informações** › **Executar assim mesmo**.
- **Mac** — “não é possível verificar o desenvolvedor”: abra **Ajustes do Sistema › Privacidade e Segurança** e clique em **Abrir Mesmo Assim** (só na primeira vez).

### Para abrir um exame

Arraste para a janela do VisualTC a pasta do CD/pendrive ou o arquivo compactado recebido (ZIP, RAR, 7z) —
ou use os botões **Abrir** e **Pasta**. No Windows, também dá para clicar com o botão direito no CD, numa pasta
ou num ZIP e escolher **Abrir no VisualTC**.

O VisualTC funciona sem internet: as imagens e os dados dos pacientes não saem do computador.
