# NieR Remastered

A graphics mod for **NieR:Automata** (Steam, 2021 version) that runs the game on **Vulkan** through a custom DXVK fork and adds **NVIDIA DLSS (DLAA)**, an **in-game settings menu** and **frame generation**, plus tools to **AI-remaster** the world textures.

[العربية ↓](#العربية)

## Features

- **Vulkan renderer**: a fork of [DXVK](https://github.com/doitsujin/dxvk) 3.1.1 with the NieR additions.
- **DLSS (DLAA)**: anti-aliasing at native resolution, with camera motion vectors and jitter injected into the game. Presets J, K, L, M.
- **In-game menu (Page Up)**: turn DLSS on/off, change the preset and frame generation. It only shows options your GPU supports. Choices are saved to `nier_mod.ini`.
- **Frame generation, 2x (RTX 50 series)**: through NVIDIA Smooth Motion. The game keeps running at 60 fps internally, so events, cutscenes and hacking keep working, while you see 120 fps.
- **Texture remaster tools**: extract, AI-upscale (Real-ESRGAN) and repack the albedo textures of buildings, trees and the world.
- **Works with ReShade** installed in Vulkan mode.
- Experimental ray traced ambient occlusion, **off** by default.

## Requirements

- Windows 10/11, NieR:Automata on Steam (the July 2021 update).
- A GPU with Vulkan 1.3.
- DLSS: an NVIDIA RTX GPU. Frame generation: an RTX 50 series GPU.

## Install

1. Download the latest zip from [Releases](../../releases) and extract it anywhere.
2. Run `install.bat`. It looks for the game in the default Steam folder and asks for the folder if it is somewhere else. You can also copy `d3d11.dll`, `dxgi.dll`, `nvngx_dlss.dll` and `dxvk.conf` next to `NieRAutomata.exe` yourself.
3. Start the game and set **Anti-Aliasing to Off** in the game's graphics settings. DLSS replaces it.
4. Press **Page Up** for the mod menu. Use **Up/Down** to select a row and **Left/Right** to change it.

The first minutes can stutter a little while Vulkan shaders are compiled. That only happens once.

**Uninstall:** run `uninstall.bat`. It removes the mod and restores any files it replaced.

## Menu

| Row | Values |
|---|---|
| DLSS (DLAA) | On / Off |
| Preset | Default, J, K, L, M |
| Frame Gen | Off / 2x Smooth Motion. Restart the game after changing it. |
| Status | waiting for gameplay, running, set the game's Anti-Aliasing to Off, error |

## Optional

- **ReShade**: install [ReShade](https://reshade.me) for `NieRAutomata.exe` and pick **Vulkan**. Its effects apply on top of the DLSS image. If you use an ambient occlusion effect (MXAO), do not stack it with another AO.
- **Not compatible** with other mods that replace `d3d11.dll` or `dxgi.dll` (FAR, Special K, stock DXVK).

## Texture remaster (advanced)

The remastered textures are **not included**. They are Square Enix's assets, and the full set is about 21 GB. You build them yourself from your own copy of the game:

1. Install Python 3.12 with `pip install numpy pillow`.
2. Put [Real-ESRGAN ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN/releases) in `tools\realesrgan\x\`, and [texconv](https://github.com/microsoft/DirectXTex/releases) in `tools\texconv\`.
3. Extract the world and building models:
   ```
   python remaster\cpk.py extract "<game>\data\data*.cpk" --match "bg/*" --out work\raw
   python remaster\cpk.py extract "<game>\data\data*.cpk" --match "wd*" --out work\raw
   python remaster\catalog.py catalog.json
   ```
4. Upscale and repack. This takes hours on the first run; results are cached in `work\upscaled`:
   ```
   python remaster\remaster.py --out out\data
   ```
5. Copy the contents of `out\data` into `<game>\data`. The game prefers loose files over its archives.

**Memory limit:** the game reserves a fixed amount of memory per world area. Without a budget the remaster stops the game in the city with `Ran out of memory while loading ...dtt`, even with [Wolf's Limit Break](https://www.nexusmods.com/nierautomata/mods/70). Build it with a budget that keeps every file at most 25% bigger:
`python remaster\remaster.py --budget 1.25 --out out\data`

## Known issues

- DLSS only knows the camera motion, not the motion of characters, so fast-moving characters can leave a slight trail.
- DLSS needs the game's Anti-Aliasing (MSAA) off. The menu tells you if it is on.

## Build from source

1. Clone with submodules: `git clone --recursive https://github.com/LuJhOn67/NieR-Remastered`
2. Get the DLSS SDK: `git clone https://github.com/NVIDIA/DLSS tools\dlss-sdk`
3. Install Visual Studio 2022 Build Tools, meson, ninja and glslang.
4. Run `dxvk-nier\build-msvc.bat`. The DLLs are written to `dxvk-nier\build.w64\src\d3d11\` and `...\dxgi\`.

## Credits and licenses

- [DXVK](https://github.com/doitsujin/dxvk) by Philip Rebohle and contributors, zlib license. `dxvk-nier` is a fork of it under the same license.
- NVIDIA DLSS SDK. `nvngx_dlss.dll` is redistributed under the NVIDIA RTX SDK license. DLSS is a trademark of NVIDIA Corporation.
- [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) (BSD-3-Clause) and [DirectXTex texconv](https://github.com/microsoft/DirectXTex) (MIT) are used by the remaster tools. They are not included.
- The mod's own scripts are under the zlib license (see `LICENSE`).
- NieR:Automata is © SQUARE ENIX. This project is not affiliated with Square Enix, PlatinumGames or NVIDIA.

---

<div dir="rtl">

## العربية

**NieR Remastered** مود رسوميات للعبة **NieR:Automata**، نسخة ستيم 2021. يشغّل اللعبة على **Vulkan** عبر نسخة معدّلة من DXVK. ويضيف إليها **DLSS (DLAA)** من NVIDIA، و**قائمة إعدادات داخل اللعبة**، و**توليد الإطارات (Frame Generation)**. ويأتي معه أيضاً أدوات **لإعادة رسم خامات العالم بالذكاء الاصطناعي (ريماستر)**.

### المميزات

- **محرّك Vulkan**: نسخة معدّلة من DXVK 3.1.1.
- **DLSS بوضع DLAA**: تنعيم الحواف بالدقة الأصلية، ومعه البريستات J وK وL وM.
- **قائمة داخل اللعبة بزر Page Up**: تشغّل منها DLSS أو تطفئه، وتغيّر البريست وتوليد الإطارات. لا تظهر فيها إلا الخيارات التي يدعمها كرت الشاشة عندك. وتُحفظ اختياراتك في `nier_mod.ini`.
- **توليد الإطارات 2x لكروت RTX 50**: عبر NVIDIA Smooth Motion. تبقى اللعبة تعمل داخلياً على 60 إطاراً، فلا تتعطل الأحداث والمشاهد السينمائية والهاكنك، بينما ترى أنت 120 إطاراً.
- **أدوات الريماستر**: تستخرج خامات البنايات والأشجار والعالم، وتكبّرها بالذكاء الاصطناعي (Real-ESRGAN)، ثم تعيد تحزيمها.
- **يعمل مع ReShade** بوضع Vulkan.
- تظليل بتتبّع الأشعة (RTAO) تجريبي، ومطفأ افتراضياً.

### المتطلبات

- ويندوز 10 أو 11، ولعبة NieR:Automata على ستيم (تحديث يوليو 2021).
- كرت شاشة يدعم Vulkan 1.3.
- لـ DLSS: كرت NVIDIA RTX. ولتوليد الإطارات: كرت من سلسلة RTX 50.

### طريقة التثبيت

1. نزّل آخر ملف zip من صفحة [Releases](../../releases)، وفكّ ضغطه في أي مكان.
2. شغّل `install.bat`. يبحث عن اللعبة في مجلد ستيم الافتراضي، وإن لم يجدها يطلب منك مسار مجلدها. ويمكنك بدلاً من ذلك نسخ الملفات `d3d11.dll` و`dxgi.dll` و`nvngx_dlss.dll` و`dxvk.conf` يدوياً إلى جانب `NieRAutomata.exe`.
3. شغّل اللعبة، واجعل **Anti-Aliasing على Off** في إعدادات الرسوميات، لأن DLSS يحل محله.
4. اضغط **Page Up** لتفتح قائمة المود. الأسهم **أعلى وأسفل** لاختيار السطر، و**يمين ويسار** لتغيير القيمة.

قد يظهر تقطيع خفيف في الدقائق الأولى أثناء تجهيز شيدرات Vulkan، وهذا يحدث مرة واحدة فقط.

**إزالة المود:** شغّل `uninstall.bat`. يحذف ملفات المود ويعيد أي ملفات استبدلها.

### القائمة

| السطر | القيم |
|---|---|
| DLSS (DLAA) | تشغيل / إطفاء |
| Preset | Default، J، K، L، M |
| Frame Gen | Off أو 2x Smooth Motion، ويحتاج إعادة تشغيل اللعبة بعد التغيير |
| Status | ينتظر بدء اللعب، يعمل، أطفئ الـ Anti-Aliasing، خطأ |

### إضافات اختيارية

- **ReShade**: ثبّت [ReShade](https://reshade.me) على `NieRAutomata.exe` واختر **Vulkan**. تُطبَّق فلاتره فوق صورة DLSS. لا تشغّل فلتر ظلال مثل MXAO مع فلتر ظلال آخر في الوقت نفسه.
- **لا يتوافق** مع أي مود آخر يستبدل `d3d11.dll` أو `dxgi.dll`، مثل FAR وSpecial K وDXVK الأصلي.

### ريماستر الخامات (للمتقدمين)

الخامات المحسّنة **غير مرفقة**، لأنها من أصول Square Enix وحجمها الكامل نحو 21 غيغابايت. تبنيها بنفسك من نسختك من اللعبة:

1. ثبّت Python 3.12، ثم نفّذ `pip install numpy pillow`.
2. ضع [Real-ESRGAN ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN/releases) في `tools\realesrgan\x\`، و[texconv](https://github.com/microsoft/DirectXTex/releases) في `tools\texconv\`.
3. شغّل أوامر الاستخراج والتكبير المكتوبة في القسم الإنجليزي أعلاه، بالترتيب نفسه.
4. انسخ محتويات `out\data` إلى `<game>\data`. اللعبة تفضّل الملفات المنفصلة على أرشيفاتها.

**حد الذاكرة:** تحجز اللعبة ذاكرة ثابتة لكل منطقة من العالم. لذلك يوقف الريماستر غير المحدود اللعبة في المدينة برسالة `Ran out of memory while loading`، حتى مع مود Wolf's Limit Break. الحل أن تبنيه بحد أقصى لا يكبر فيه أي ملف بأكثر من 25%، بإضافة `--budget 1.25` إلى أمر `remaster.py`.

### مشاكل معروفة

- يعرف DLSS حركة الكاميرا فقط، لا حركة الشخصيات، فقد يظهر أثر خفيف خلف الشخصيات السريعة.
- يحتاج DLSS أن يكون الـ Anti-Aliasing في اللعبة مطفأً، والقائمة تنبّهك إن كان مشغّلاً.

### الحقوق

المود مبني على DXVK (رخصة zlib)، ويستخدم DLSS SDK من NVIDIA. لعبة NieR:Automata ملك لشركة SQUARE ENIX، وهذا المشروع غير تابع لها ولا لشركتي PlatinumGames وNVIDIA.

</div>
