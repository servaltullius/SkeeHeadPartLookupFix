# 넥서스 업로드 안내 (Skee HeadPart Lookup Fix 2.1.0)

## 1. 기본 정보 (Mod details)

| 항목 | 넣을 값 |
|---|---|
| Mod name | `Skee HeadPart Lookup Fix` |
| Version | `2.1.0` |
| Category | **Bug Fixes** (OverlayFix, cosave Load Crash Fix, Hitch Hunter 와 같은 분류) |
| Language | English |
| Author | 본인 닉네임 |
| Brief overview (요약, 350자 제한) | 아래 문구 |

요약 문구:
```
Removes a slow head part lookup in RaceMenu (skee64.dll) that causes stutter when a sculpted face is rebuilt in load orders with many head parts. Same result as the original, answered from a table. RaceMenu 0.4.20.0 Steam/GOG, 0.4.19.16 (AE) and 0.4.16 (SE).
```

## 2. 설명 (Description)

`description.bbcode` 내용을 통째로 붙여 넣는다. 에디터는 BBCode 모드로 둔다.

## 3. 태그 (Tags)

- Performance Optimization
- Bug Fixes
- Skyrim Script Extender (SKSE)
- Utilities for Players
- **AI-Generated Content**
  - 이 플러그인은 AI 도움으로 분석·작성했다.
  - 넥서스에 이 태그가 있고, Hitch Hunter 도 달았다.
  - 넥서스의 현재 AI 콘텐츠 규정을 확인한 뒤, 표시가 필요하면 단다.

## 4. 요구 사항 (Requirements)

| Nexus 모드 | ID | 메모 |
|---|---|---|
| Skyrim Script Extender (SKSE64) | 30379 | |
| Address Library for SKSE Plugins | 32444 | |
| RaceMenu | 19080 | `0.4.20.0 (Steam/GOG), 0.4.19.16 or 0.4.16 (SE)` |

- DLC: 필요 없음.
- VR 은 지원하지 않는다. VR 용 Address Library 는 넣지 않는다.

## 5. 파일 (Files)

| 항목 | 값 |
|---|---|
| File | `G:\skyrim-build\SkeeHeadPartLookupFix\SkeeHeadPartLookupFix-2.1.0.zip` (DLL 하나) |
| File name | `Skee HeadPart Lookup Fix` |
| Version | `2.1.0` |
| Category | Main Files |
| Description | `SE 1.5.97 and AE 1.6.1170 / GOG. One DLL, all supported RaceMenu builds.` |

- zip 은 `SKSE\Plugins\SkeeHeadPartLookupFix.dll` 구조라 MO2/Vortex 에서 바로 설치된다.
- 소스 코드: 아래 7번 참고.

## 6. 권한 (Permissions) — 본인이 정할 것

흔한 선택은 다음과 같다.
- Upload permission: "You must get permission"
- Modification / Conversion: 허용 + 크레딧 표기 조건
- Asset use: 해당 없음(에셋 없음)
- Assets from others: **RaceMenu by Expired** 를 크레딧에 적는다.
  - 이 모드는 RaceMenu 파일을 포함하지도 수정하지도 않고, 실행 중 메모리만 바꾼다.

## 7. 소스 코드 (선택)

설명에서는 "source available" 문구를 뺐다. 공개 방법을 정하면 Credits 아래에 한 줄을 추가한다.
- 바이너리 패치 DLL 은 소스를 공개하면 신뢰를 얻기 쉽다(OverlayFix, cosave Load Crash Fix 도 공개).
- 방법은 둘 중 하나다.
  - 소스 zip 을 Optional/Miscellaneous 파일로 올린다.
  - GitHub 저장소 링크를 건다.

## 8. 변경 기록 (Changelog)

```
2.1.0 - First public release.
- Supports RaceMenu 0.4.20.0 (Steam, GOG), 0.4.19.16 (AE) and 0.4.16 (SE).
- Sculpt head part lookup and GetHeadPartByName answered from a name table (same first-match result).
- Refuses to patch unless the exact skee64.dll build and every patched instruction match.
```

## 9. 올리기 전 체크

- [ ] zip 의 DLL 이 게임에서 확인한 빌드인가 (2026-10-23 14:02 빌드, 14:08 실행 확인)
- [ ] 설명 미리보기에서 BBCode 가 깨지지 않는가
- [ ] 요구 사항 3개를 연결했는가
- [ ] Permissions / Credits 에 Expired(RaceMenu) 가 들어갔는가
