# Licenças de terceiros

O VisualTC usa os componentes abaixo. Os textos completos das licenças estão
nos repositórios de origem indicados; quem redistribuir o VisualTC deve
incluir este arquivo e os textos completos junto aos pacotes.

**This software is based in part on the work of the Independent JPEG Group.**

## Componentes distribuídos com o aplicativo

| Componente | Versão | Licença | Forma de uso |
|---|---|---|---|
| Qt (qtbase, qtsvg, qtimageformats) | 6.8.3 | GNU LGPL v3 | bibliotecas dinâmicas, sem modificações |
| GDCM — Grassroots DICOM | 3.0.24 | BSD-3-Clause | biblioteca estática (aplicativo e `visualtc-worker`) |
| OpenJPEG (incluído no GDCM; via vcpkg no CI) | 2.3 / 2.5 | BSD-2-Clause | JPEG 2000 |
| CharLS (incluído no GDCM) | 2.0 | BSD-3-Clause | JPEG-LS |
| IJG libjpeg 6b, variantes 8/12/16 bits (incluído no GDCM) | 6b | IJG License | JPEG |
| zlib (incluído no GDCM; via vcpkg no CI) | 1.3 | zlib License | Deflate |
| Expat (incluído no GDCM; via vcpkg no CI) | 2.x | MIT | XML do dicionário |
| utfcpp (incluído no GDCM) | — | Boost Software License 1.0 | conversão UTF-8 |
| libuuid (incluído no GDCM) | — | BSD-3-Clause | geração de UIDs (não usada para gravar arquivos) |
| md5 (L. Peter Deutsch, incluído no GDCM) | — | zlib-like | — |

### Avisos de copyright

- **GDCM**: Copyright (c) 2006-2016 Mathieu Malaterre; Copyright (c)
  1993-2005 CREATIS. BSD-3-Clause. <https://github.com/malaterre/GDCM>
- **OpenJPEG**: Copyright (c) 2002-2014 Université catholique de Louvain
  (UCL), Belgium; Professor Benoit Macq; Antonin Descampe; e demais autores
  listados no arquivo LICENSE. BSD-2-Clause. <https://github.com/uclouvain/openjpeg>
- **CharLS**: Copyright (c) 2007-2010 Jan de Vaan. BSD-3-Clause.
  <https://github.com/team-charls/charls>
- **IJG libjpeg**: Copyright (C) 1991-1998 Thomas G. Lane. IJG License
  (exige a frase "This software is based in part on the work of the
  Independent JPEG Group" na documentação, presente acima).
- **zlib**: Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler.
- **Expat**: Copyright (c) 1998-2000 Thai Open Source Software Center Ltd
  and Clark Cooper; Copyright (c) 2001-2006 Expat maintainers. MIT.
- **utfcpp**: Copyright 2006 Nemanja Trifunovic. BSL-1.0.
- **md5**: Copyright (C) 1999, 2002 Aladdin Enterprises.

## Qt e a LGPL v3 — obrigações de quem distribui

O VisualTC liga o Qt **dinamicamente** e não o modifica. Ao distribuir os
pacotes (instalador Windows, DMG, AppImage, .deb), é preciso:

1. Incluir o texto da LGPL v3 e da GPL v3
   (<https://www.gnu.org/licenses/lgpl-3.0.html>) e este aviso.
2. Informar que o aplicativo usa o Qt sob a LGPL v3 e indicar onde obter o
   código-fonte do Qt usado (<https://download.qt.io/archive/qt/6.8/6.8.3/>),
   ou oferecê-lo por escrito.
3. Permitir que o usuário substitua as bibliotecas do Qt por outra versão
   compatível (atendido pela ligação dinâmica: DLLs na pasta do programa,
   frameworks no `.app`, `.so` no AppImage/.deb).
4. Não impor termos que proíbam engenharia reversa para depuração dessas
   modificações.

O Qt contém componentes de terceiros próprios (FreeType, HarfBuzz, libpng,
libjpeg-turbo, PCRE2, md4c, double-conversion, entre outros), com licenças
permissivas listadas em <https://doc.qt.io/qt-6/licenses-used-in-qt.html>.

## Usados apenas no desenvolvimento (não distribuídos)

| Componente | Versão | Licença |
|---|---|---|
| Catch2 | 3.7.1 | Boost Software License 1.0 |
| CMake, Ninja, vcpkg | — | BSD-3 / Apache-2.0 / MIT |
| linuxdeploy, Inno Setup | — | MIT / licença Inno Setup |

## Planejados (ainda não incluídos)

| Componente | Versão prevista | Licença | Uso |
|---|---|---|---|
| VTK | 9.3 | BSD-3-Clause | volume rendering 3D (v1.0) |
| DCMTK | 3.6.8 | BSD-3-Clause (e licenças dos módulos) | comunicação PACS |
| ITK | 5.4 | Apache-2.0 | processamento avançado, se necessário |

## Ícones e recursos

Os ícones em `resources/icons/` e o ícone do aplicativo foram desenhados
para o VisualTC (sem uso de conjuntos de ícones de terceiros).
