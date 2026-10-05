# Compilação, testes e distribuição

## 1. Requisitos

| Item | Versão |
|---|---|
| CMake | ≥ 3.25 com presets (3.21 sem presets) |
| Ninja | recomendado (Linux/macOS) |
| Compilador C++20 | MSVC 2022 17.8+, Apple Clang 15+, Clang 14+, GCC 11+ |
| Qt | 6.5 ou superior; **fixado em 6.8.3 LTS** no CI (módulos qtbase, qtsvg, qtimageformats) |
| vcpkg | baseline `9e593bb18ea69cc5095e012465dcd675a822ed0d` (fixa GDCM 3.0.24 e Catch2 3.7.1) |

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
| `linux-debug-asan` | Linux | Debug + AddressSanitizer + UndefinedBehaviorSanitizer |
| `linux-local` | Linux sem vcpkg | dependências em `/opt/visualtc-deps` (ver §3) |
| `windows-msvc-release` | Windows 10/11 x64 | Visual Studio 2022, `x64-windows` |
| `macos-arm64-release` | macOS 12+ Apple Silicon | `arm64-osx`, `CMAKE_OSX_ARCHITECTURES=arm64` |
| `macos-x86_64-release` | macOS 12+ Intel | `x64-osx` |

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

`scripts/build_deps_linux.sh` compila GDCM 3.0.24, Catch2 3.7.1 e Qt 6.8.3
(qtbase, qtsvg, qtimageformats) a partir das tags oficiais:

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
  protocolo do worker, sincronização.
- `visualtc_ui_tests` (QtTest, `QT_QPA_PLATFORM=offscreen`): janela real,
  arrasto de W/L, roda/teclado, régua em mm, ROI em HU, sincronização,
  crosshair do MPR, recuperação de queda do worker, arquivo com falha sem
  laço de decodificação, multiframe maior que o cache e, na janela
  principal, atalhos sem duplicidade e preset de TC pela tecla 1.
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

1. Matriz Ubuntu 22.04, Windows Server 2022, macOS 14 (ARM) e macOS 13
   (Intel): configuração, compilação, testes, teste de fumaça (gera o exame
   sintético, abre o MPR e salva uma captura) e empacotamento.
2. Ubuntu 24.04: testes sob ASan/UBSan e `clang-tidy` no núcleo
   (`.clang-tidy`).

Os pacotes ficam como artefatos do workflow (`VisualTC-<preset>`).

## 6. Empacotamento

### Linux — `VisualTC-x86_64.AppImage` e `visualtc_amd64.deb`

```bash
cmake --install build/linux-release --prefix AppDir/usr
export QMAKE=$QT_ROOT_DIR/bin/qmake EXTRA_PLATFORM_PLUGINS=libqwayland-generic.so
linuxdeploy-x86_64.AppImage --appdir AppDir \
  --executable AppDir/usr/bin/VisualTC --executable AppDir/usr/bin/visualtc-worker \
  --desktop-file packaging/linux/visualtc.desktop --icon-file packaging/linux/visualtc.png \
  --plugin qt --output appimage
```

O AppImage é gerado no Ubuntu 22.04 (glibc 2.35) para rodar nas LTS mais
recentes. O `.deb` reaproveita a mesma árvore autocontida em
`/opt/visualtc` (Qt 6.8 privado, pois o Ubuntu LTS traz Qt 6.2/6.4), com
`/usr/bin/visualtc`, entrada de menu e ícone — ver o passo "Package (Linux)"
do workflow. Em distribuições com Qt ≥ 6.5 no sistema, `cpack -G DEB` gera
um pacote que usa o Qt da distribuição.

### Windows — `VisualTC-Setup-x64.exe`

```powershell
mkdir stage; copy build\windows-msvc-release\bin\Release\*.exe stage\
copy build\windows-msvc-release\bin\Release\*.dll stage\   # DLLs do vcpkg, se houver
& "$env:QT_ROOT_DIR\bin\windeployqt.exe" --release --no-opengl-sw stage\VisualTC.exe
copy README.md, THIRD_PARTY_LICENSES.md stage\
ISCC.exe /DSourceDir=%CD%\stage /DOutputDir=%CD%\dist packaging\windows\visualtc.iss
```

O instalador (Inno Setup 6, português e inglês) instala por usuário ou para
todos, cria atalhos e um desinstalador. **Assinatura (recomendada para
evitar o SmartScreen):**

```powershell
signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /a stage\VisualTC.exe stage\visualtc-worker.exe
# depois de gerar o instalador:
signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /a dist\VisualTC-Setup-x64.exe
```

### macOS — `VisualTC.dmg`

O `visualtc-worker` é copiado para `VisualTC.app/Contents/MacOS` na
compilação. Para distribuição:

```bash
APP=build/macos-arm64-release/bin/VisualTC.app
$QT_ROOT_DIR/bin/macdeployqt "$APP" -executable="$APP/Contents/MacOS/visualtc-worker"
# Assinatura Developer ID com hardened runtime (o worker é assinado junto)
codesign --force --options runtime --timestamp \
  --sign "Developer ID Application: <Nome> (<TEAMID>)" "$APP/Contents/MacOS/visualtc-worker"
codesign --force --deep --options runtime --timestamp \
  --sign "Developer ID Application: <Nome> (<TEAMID>)" "$APP"
hdiutil create -volname VisualTC -srcfolder "$APP" -ov -format UDZO VisualTC.dmg
codesign --sign "Developer ID Application: <Nome> (<TEAMID>)" --timestamp VisualTC.dmg
# Notarização
xcrun notarytool submit VisualTC.dmg --keychain-profile "<perfil>" --wait
xcrun stapler staple VisualTC.dmg
```

Sem certificado, o CI aplica uma assinatura ad hoc (`codesign --sign -`):
o app abre com "clique direito › Abrir" na primeira vez. Um binário
universal (arm64 + x86_64) pode ser feito com `lipo` a partir dos dois
presets; o CI publica um DMG por arquitetura.

## 7. Solução de problemas

| Sintoma | Causa provável |
|---|---|
| `Could not find a package configuration file provided by "GDCM"` | `VCPKG_ROOT` não definido ou preset sem vcpkg |
| `Could not find Qt6` | `QT_ROOT_DIR` não aponta para a pasta do kit (ex.: `.../6.8.3/gcc_64`) |
| `qt.qpa.plugin: Could not load the Qt platform plugin "xcb"` | instalar `libxcb-cursor0` (Ubuntu) |
| Log: "Decodificador isolado não encontrado" | `visualtc-worker` não está ao lado do executável (no macOS, dentro de `Contents/MacOS`) |
| Testes de interface falham no Linux sem tela | exporte `QT_QPA_PLATFORM=offscreen` (os presets de teste já fazem isso) |
