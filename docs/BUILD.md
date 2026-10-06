# Compilação, testes e distribuição

## 1. Requisitos

| Item | Versão |
|---|---|
| CMake | ≥ 3.25 com presets (3.21 sem presets) |
| Ninja | recomendado (Linux/macOS) |
| Compilador C++20 | MSVC 2022 17.8+, Apple Clang 15+, Clang 14+, GCC 11+ |
| Qt | 6.5 ou superior; **fixado em 6.8.3 LTS** no CI (módulos qtbase, qtsvg, qtimageformats) |
| vcpkg | baseline `9e593bb18ea69cc5095e012465dcd675a822ed0d` (fixa GDCM 3.0.24, Catch2 3.7.1 e a libarchive 3.8.x da baseline) |

As dependências de terceiros e o motivo de cada uma estão em
[ARCHITECTURE.md](ARCHITECTURE.md#d-dependências-e-versões-fixadas).

## 2. Compilação com vcpkg (os três sistemas)

```bash
git clone https://github.com/microsoft/vcpkg && ./vcpkg/bootstrap-vcpkg.sh   # .bat no Windows
export VCPKG_ROOT=$PWD/vcpkg
# Qt 6.8.3: instalador oficial, aqt (pip install aqtinstall) ou Homebrew
export QT_ROOT_DIR=$HOME/Qt/6.8.3/gcc_64           # macos | msvc2022_64
cmake --preset linux-release                         # ver tabela abaixo
cmake --build --preset linux-release --parallel
ctest --preset linux-release
```

| Preset | Sistema | Observações |
|---|---|---|
| `linux-release` | Ubuntu 22.04+ x86_64 | Ninja, Release, vcpkg `x64-linux` |
| `linux-debug-asan` | Linux | Debug + AddressSanitizer + UndefinedBehaviorSanitizer no código do VisualTC; bibliotecas de terceiros em Release (as asserções de depuração do GDCM abortam com arquivos corrompidos de propósito) |
| `linux-local` | Linux sem vcpkg | dependências em `/opt/visualtc-deps` (ver §3) |
| `windows-msvc-release` | Windows 10/11 x64 | Visual Studio 2022, `x64-windows` |
| `macos-arm64-release` | macOS 12+ Apple Silicon | triplet `arm64-osx-visualtc` (`cmake/triplets`: dependências também para macOS 12) |
| `macos-x86_64-release` | macOS 12+ Intel | triplet `x64-osx-visualtc` |

No Windows, use o "x64 Native Tools Command Prompt for VS 2022" (ou o
PowerShell com `VsDevShell`) e `set QT_ROOT_DIR=C:\Qt\6.8.3\msvc2022_64`.
Os executáveis ficam em `build/<preset>/bin` (`bin/Release` no Windows;
`bin/VisualTC.app` no macOS).

Opções do CMake:

| Opção | Padrão | Efeito |
|---|---|---|
| `VISUALTC_BUILD_APP` | ON | compila a interface (desligue para compilar só o núcleo e o worker, sem Qt) |
| `VISUALTC_BUILD_TESTS` | ON | testes Catch2 + QtTest |
| `VISUALTC_ENABLE_SANITIZERS` | OFF | ASan + UBSan (GCC/Clang) |
| `VISUALTC_WARNINGS_AS_ERRORS` | OFF | `-Werror` / `/WX` |
| `VISUALTC_FETCH_CATCH2` | ON | baixa o Catch2 3.7.1 se não for encontrado |
| `VISUALTC_PACKAGE_CONTACT` | placeholder | campo Maintainer dos pacotes Linux |

## 3. Compilação sem vcpkg (Linux)

`scripts/build_deps_linux.sh` compila GDCM 3.0.24, zstd 1.5.7, libarchive
3.8.7, Catch2 3.7.1 e Qt 6.8.3 (qtbase, qtsvg, qtimageformats) a partir das
tags oficiais (precisa dos pacotes de desenvolvimento de zlib, bzip2, liblzma
e OpenSSL; no Ubuntu: `zlib1g-dev libbz2-dev liblzma-dev libssl-dev`):

```bash
scripts/build_deps_linux.sh /opt/visualtc-deps          # Qt completo (desktop)
scripts/build_deps_linux.sh --headless /opt/visualtc-deps  # só plataforma offscreen (CI/containers)
cmake --preset linux-local && cmake --build --preset linux-local && ctest --preset linux-local
```

Foi assim que o VisualTC foi compilado e testado no ambiente de
desenvolvimento desta versão (Qt headless; a interface foi verificada com
capturas de tela offscreen).

## 4. Testes

```bash
ctest --preset linux-release              # tudo (Catch2 + interface offscreen)
build/linux-release/tests/visualtc_tests "[mpr]"     # por etiqueta
build/linux-release/tests/visualtc_tests --list-tags
```

- `visualtc_tests` (Catch2): texto/charsets, VOI/HU, medidas, ROIs,
  orientação, ordenação e geometria, leitura DICOM em todos os codecs,
  arquivos hostis, MPR com fantomas lineares (resultado exato), cache,
  protocolo do worker, sincronização e exames compactados (`[archive]`: todos
  os formatos, nomes maliciosos, aninhados, senha, bombas de compressão,
  arquivos danificados).
- `visualtc_ui_tests` (QtTest, `QT_QPA_PLATFORM=offscreen`): janela real,
  arrasto de W/L, roda/teclado, régua em mm, ROI em HU, sincronização,
  crosshair do MPR, recuperação de queda do worker, arquivo com falha sem
  laço de decodificação, multiframe maior que o cache, ZIP com senha (AES),
  limpeza da pasta temporária e, na janela principal, atalhos sem
  duplicidade, preset de TC pela tecla 1 e o painel de séries recolhível.
- O teste `DS parsing does not depend on the process locale` precisa de uma
  localidade com vírgula decimal instalada (`sudo locale-gen pt_BR.UTF-8` no
  Ubuntu); sem ela, é marcado como ignorado.
- Fuzzing opcional dos codecs (oculto por padrão; rode sob ASan):

  ```bash
  VTC_FUZZ_TRIALS=2000 VTC_FUZZ_SEED=7 build/linux-debug-asan/tests/visualtc_tests "[fuzz]"
  ```

- Desempenho: `build/<preset>/tests/vtc_bench <pasta>` (varredura,
  decodificação por codec, W/L, montagem do volume e MPR).
- Exame sintético para testes manuais: `build/<preset>/tests/make_phantom <pasta> [--small]`.

O executável também tem opções de automação usadas no CI:
`VisualTC --size 1600x1000 --layout 2x2 --sync --mpr --slab 20 --preset Pulmão --screenshot saida.png <pasta>`.

## 5. Integração contínua

`.github/workflows/build.yml` executa, a cada push/PR:

| Job | Runner | O que faz |
|---|---|---|
| Ubuntu x86_64 | `ubuntu-22.04` (glibc 2.35) | compila, testa, gera `.deb` + AppImage, **instala o `.deb` com apt** e abre um exame com ele e com o AppImage numa tela virtual (Xvfb, plugin X11 real), sem o Qt do CI no caminho; confere o runtime estático do AppImage |
| Windows x64 | `windows-2022` | compila, testa, gera o instalador, **instala em modo silencioso**, abre um exame com a cópia instalada (sem o Qt do CI no PATH), confere o menu de contexto e desinstala |
| macOS Apple Silicon | `macos-15` | compila, testa, aplica `macdeployqt` |
| macOS Intel | `macos-15-intel` | idem, nativo em x86_64 |
| macOS DMG | `macos-15` | junta os dois apps em um **app universal** (`lipo`), assina, gera o DMG e o testa nas duas arquiteturas |
| ASan/UBSan | `ubuntu-24.04` | testes sob sanitizers e `clang-tidy` no núcleo |

Os pacotes ficam como artefatos do workflow. Datas a acompanhar: o GitHub
remove a imagem `ubuntu-22.04` em abril de 2027 (trocar por `ubuntu-24.04`;
os pacotes passam a exigir Ubuntu 24.04 / Debian 13) e oferece a imagem Intel
`macos-15-intel` até 2027.

### Publicar uma versão

1. Atualize `VERSION` em `CMakeLists.txt` (o instalador do Windows, o `.deb`
   e o app do macOS leem a versão daí) e `version` em `vcpkg.json`.
2. Crie a versão de um destes jeitos (o job confere que a tag e o
   `VERSION` do CMakeLists.txt coincidem):
   - no site do GitHub: *Releases › Draft a new release*, tag nova `v0.2.0`,
     *Publish release* — os instaladores são anexados quando o workflow
     termina (a primeira compilação leva cerca de uma hora);
   - ou pelo terminal: `git tag v0.2.0 && git push origin v0.2.0`.
3. O job **release** publica a versão no GitHub com nomes fixos:
   `VisualTC-Setup-x64.exe`, `VisualTC-macOS.dmg`, `visualtc_amd64.deb`,
   `VisualTC-x86_64.AppImage` e `SHA256SUMS.txt`, com as instruções de
   instalação em português (`packaging/release-notes.md`).

Como os nomes não mudam, `https://github.com/<dono>/<repo>/releases/latest/download/<arquivo>`
aponta sempre para a versão mais nova — é o que usam a seção "Baixar" do
README e a página de download.

**Página de download.** `site/index.html` (português, sem dependências
externas) detecta o sistema do visitante e destaca o botão certo, com os
passos de instalação de cada sistema. O workflow `pages.yml` a publica em
`https://<dono>.github.io/<repo>/`; ative uma vez em *Settings › Pages ›
Source: GitHub Actions*.

### Assinatura (opcional, recomendada)

Sem certificados, tudo funciona, mas na primeira abertura o Windows mostra o
SmartScreen ("Mais informações › Executar assim mesmo") e o macOS pede
confirmação em *Ajustes do Sistema › Privacidade e Segurança*. Para que o
programa abra com um simples clique, cadastre estes *secrets* no repositório
(*Settings › Secrets and variables › Actions*):

| Secret | Uso |
|---|---|
| `MACOS_CERTIFICATE_P12` | certificado "Developer ID Application" exportado (.p12) em base64 |
| `MACOS_CERTIFICATE_PASSWORD` | senha do .p12 |
| `MACOS_SIGN_IDENTITY` | ex.: `Developer ID Application: Nome (TEAMID)` |
| `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` | notarização (`notarytool`; senha de app gerada em appleid.apple.com) |
| `WINDOWS_CERTIFICATE_PFX`, `WINDOWS_CERTIFICATE_PASSWORD` | certificado Authenticode (.pfx em base64) |

O certificado da Apple exige a conta Apple Developer (anual). No Windows, os
certificados emitidos desde 2023 ficam em token/HSM e não podem ser
exportados como .pfx; nesse caso, assine com o serviço de assinatura em
nuvem da Microsoft (Azure) ou da autoridade certificadora, adaptando a função
`Invoke-Sign` de `packaging/windows/make_installer.ps1`.

## 6. Empacotamento

Cada sistema tem um script, o mesmo usado pelo CI.

### Linux — `visualtc_amd64.deb` e `VisualTC-x86_64.AppImage`

```bash
packaging/linux/make_packages.sh build/linux-release "$QT_ROOT_DIR/bin/qmake" dist
```

O script baixa linuxdeploy, o plugin Qt e o appimagetool (ou usa os que
estiverem em `TOOLS_DIR`), monta a árvore autocontida (Qt 6.8, GDCM e
libarchive embutidos) e gera:

- `.deb`: instala em `/opt/visualtc`, com `/usr/bin/visualtc`, entrada de
  menu e ícone. Clique duplo abre a Central de Programas (Ubuntu, Mint,
  Debian com GNOME Software); `apt` resolve as dependências do sistema.
- Plataformas do Qt incluídas: X11 (`xcb`) e `offscreen`. Em sessões
  Wayland o VisualTC roda pelo XWayland; para incluir o plugin Wayland é
  preciso também o módulo de integração de shell
  (`EXTRA_QT_MODULES=waylandcompositor` no linuxdeploy-plugin-qt).
- AppImage: roda em qualquer distribuição x86_64 com glibc ≥ 2.35, sem
  instalar; usa o *runtime* estático do appimagetool, então **não precisa de
  libfuse2**. O usuário só precisa marcar "Permitir executar como programa".

Em distribuições com Qt ≥ 6.5 no sistema, `cpack -G DEB` gera um pacote que
usa o Qt da distribuição.

### Windows — `VisualTC-Setup-x64.exe`

```powershell
pwsh packaging/windows/make_installer.ps1 -BuildDir build/windows-msvc-release -OutDir dist
```

O script junta os executáveis, as DLLs do vcpkg, o Qt (`windeployqt`) e o
**runtime do Visual C++** (cópia local, para abrir em PCs sem o
"Visual C++ Redistributable"), assina se houver certificado e chama o Inno
Setup 6 (`packaging/windows/visualtc.iss`). O instalador:

- não pede senha de administrador (instala para o usuário em
  `%LOCALAPPDATA%\Programs\VisualTC`; `/ALLUSERS` instala para todos);
- escolhe português ou inglês pelo idioma do Windows, sem perguntar;
- cria atalhos no menu Iniciar e na área de trabalho e abre o VisualTC no fim;
- opcionalmente adiciona "Abrir no VisualTC" ao clicar com o botão direito
  em pastas, unidades (CD/DVD, pendrive) e arquivos ZIP/RAR/7z/ISO, e
  associa `.dcm` (aparece em "Abrir com");
- instalação silenciosa para TI: `VisualTC-Setup-x64.exe /VERYSILENT /ALLUSERS`.

### macOS — `VisualTC-macOS.dmg` (universal)

```bash
# em cada Mac (ou runner): Apple Silicon e Intel
APP=build/macos-arm64-release/bin/VisualTC.app
$QT_ROOT_DIR/bin/macdeployqt "$APP" -executable="$APP/Contents/MacOS/visualtc-worker"
# depois, com os dois apps lado a lado (requer: pip install dmgbuild)
packaging/macos/make_dmg.sh VisualTC-macOS.dmg arm64/VisualTC.app x86_64/VisualTC.app
```

`make_dmg.sh` combina com `lipo` os executáveis das duas arquiteturas (os
frameworks e plugins do Qt já são universais), confere que **todo** código
do pacote roda em arm64 e x86_64, assina de dentro para fora (Developer ID
com *hardened runtime* se `MACOS_SIGN_IDENTITY` estiver definido; senão,
assinatura ad hoc), gera o DMG com `dmgbuild` — janela com o app, uma seta e
o atalho **Aplicativos**, e a instrução "Arraste o VisualTC para a pasta
Aplicativos" no fundo (`tools/make_dmg_background.py`) — e, com as
credenciais da Apple, notariza e grampeia (`stapler`) o DMG. Com um único
app, gera um DMG daquela arquitetura.

O app declara `.dcm`, arquivos compactados e pastas: aparece em "Abrir com"
no Finder e aceita a pasta do CD arrastada para o ícone no Dock.

## 7. Solução de problemas

| Sintoma | Causa provável |
|---|---|
| `Could not find a package configuration file provided by "GDCM"` | `VCPKG_ROOT` não definido ou preset sem vcpkg |
| `Could not find Qt6` | `QT_ROOT_DIR` não aponta para a pasta do kit (ex.: `.../6.8.3/gcc_64`) |
| `qt.qpa.plugin: Could not load the Qt platform plugin "xcb"` | instalar `libxcb-cursor0` (Ubuntu) |
| Log: "Decodificador isolado não encontrado" | `visualtc-worker` não está ao lado do executável (no macOS, dentro de `Contents/MacOS`) |
| Testes de interface falham no Linux sem tela | exporte `QT_QPA_PLATFORM=offscreen` (os presets de teste já fazem isso) |
