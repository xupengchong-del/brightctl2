#!/bin/bash
# test.sh —— brightctl2 自动化验收
# 会真实读写亮度，结束前把每台屏还原到起始值。
# 断言依赖中文输出，因此固定语言；另有一段英文冒烟检查。
set -u
export BRIGHTCTL_LANG=zh
BIN="${BIN:-./brightctl2}"
pass=0; fail=0
ok()   { echo "  ✓ $1"; pass=$((pass+1)); }
bad()  { echo "  ✗ $1"; fail=$((fail+1)); }
check_eq() { [ "$1" = "$2" ] && ok "$3 ($1)" || bad "$3: 期望 $2，实际 $1"; }
check_range() { # 值 下限 上限 描述
  awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN{exit !(v>=lo && v<=hi)}' \
    && ok "$4 ($1)" || bad "$4: $1 不在 [$2,$3]"
}

echo "== 1. 枚举 =="
LIST="$($BIN)"
echo "$LIST"
ID_COUNT=$(printf '%s\n' "$LIST" | grep -c 'display id=')
[ "$ID_COUNT" -ge 1 ] && ok "枚举到 $ID_COUNT 台屏" || bad "枚举不到屏"
CONTROLLABLE=$(printf '%s\n' "$LIST" | grep 'display id=' | grep -vc 'channel=不可控')
[ "$CONTROLLABLE" -ge 1 ] && ok "$CONTROLLABLE 台可控" || bad "没有可控屏"

declare -a IDS RESTORE
i=0
while read -r id; do
  v="$($BIN get "$id" 2>/dev/null)"
  if [ -n "$v" ]; then IDS[$i]="$id"; RESTORE[$i]="$v"; i=$((i+1)); fi
done < <(printf '%s\n' "$LIST" | grep 'display id=' | grep -v 'channel=不可控' | sed -n 's/.*id=\(0x[0-9a-f]*\).*/\1/p')

restore_all() {
  for j in "${!IDS[@]}"; do
    "$BIN" set "${IDS[$j]}" "$(printf '%.0f' "${RESTORE[$j]}")" >/dev/null 2>&1
  done
}
trap restore_all EXIT

echo "== 2. get 合法性 =="
for j in "${!IDS[@]}"; do
  v="$($BIN get "${IDS[$j]}")"
  check_range "$v" 0 100 "get ${IDS[$j]}"
done

echo "== 3. set 绝对/相对读写闭环 =="
for j in "${!IDS[@]}"; do
  id="${IDS[$j]}"; start="$(printf '%.0f' "${RESTORE[$j]}")"
  if [ "$start" -ge 50 ]; then target=$((start-4)); rel=-3; else target=$((start+4)); rel=+3; fi
  "$BIN" set "$id" "$target" >/dev/null
  after="$($BIN get "$id")"
  check_range "$after" $((target-1)) $((target+1)) "set $id $target 后读回"
  "$BIN" set "$id" "$rel" >/dev/null
  after2="$($BIN get "$id")"
  check_range "$after2" $((target+rel-1)) $((target+rel+1)) "set $id $rel 相对增减"
done

echo "== 4. 边界 clamp =="
id0="${IDS[0]}"
"$BIN" set "$id0" 100 >/dev/null; v100="$($BIN get "$id0")"
check_range "$v100" 99 100 "set 100 上限"
"$BIN" set "$id0" 0 >/dev/null; v0="$($BIN get "$id0")"
check_range "$v0" 0 1 "set 0 下限"

echo "== 5. up/down =="
restore_all; sleep 0.3
base="$($BIN get "$id0")"; basei="${base%.*}"
"$BIN" up 6 >/dev/null; upv="$($BIN get "$id0")"
check_range "$upv" $((basei+5)) $((basei+7)) "up 6 生效"
"$BIN" down 6 >/dev/null; back="$($BIN get "$id0")"
check_range "$back" $((basei-1)) $((basei+1)) "down 6 还原"

echo "== 6. sync 一致性 =="
"$BIN" sync >/dev/null
ref="$($BIN get "$id0")"; refi="${ref%.*}"
for j in "${!IDS[@]}"; do
  v="$($BIN get "${IDS[$j]}")"
  check_range "$v" $((refi-2)) $((refi+2)) "sync 后 ${IDS[$j]} 与参考屏一致"
done

echo "== 7. all =="
"$BIN" all 40 >/dev/null
for j in "${!IDS[@]}"; do
  v="$($BIN get "${IDS[$j]}")"
  check_range "$v" 39 41 "all 40 → ${IDS[$j]}"
done

echo "== 8. 参数与错误处理 =="
"$BIN" get >/dev/null 2>&1; check_eq "$?" 2 "get 缺参数退出码"
"$BIN" set 0x2 >/dev/null 2>&1; check_eq "$?" 2 "set 缺参数退出码"
"$BIN" set 0x2 abc >/dev/null 2>&1; check_eq "$?" 2 "set 非数字退出码"
"$BIN" set 0x2 150 >/dev/null 2>&1; check_eq "$?" 2 "set 超范围退出码"
"$BIN" get 0xffff >/dev/null 2>&1; check_eq "$?" 1 "get 不存在 id 退出码"
"$BIN" bogus >/dev/null 2>&1; check_eq "$?" 2 "未知命令退出码"
"$BIN" --help >/dev/null 2>&1; check_eq "$?" 0 "--help 退出码"

echo "== 9. sys 系统状态 =="
SYS_ZH="$($BIN --lang zh sys 2>&1)"
printf '%s\n' "$SYS_ZH" | grep -q '系统状态' && ok "sys 标题" || bad "sys 标题异常: $SYS_ZH"
printf '%s\n' "$SYS_ZH" | grep -Eq 'CPU +[0-9]+\.[0-9]%' && ok "sys CPU 数值" || bad "sys CPU 异常"
printf '%s\n' "$SYS_ZH" | grep -Eq '内存 +[0-9]+\.[0-9]%' && ok "sys 内存数值" || bad "sys 内存异常"
printf '%s\n' "$SYS_ZH" | grep -Eq '负载 [0-9]' && ok "sys 负载数值" || bad "sys 负载异常"
printf '%s\n' "$SYS_ZH" | grep -Eq 'GPU +[0-9]+' && ok "sys GPU 数值" || bad "sys GPU 异常"
printf '%s\n' "$SYS_ZH" | grep -Eq '磁盘 +[0-9]+\.[0-9]%' && ok "sys 磁盘数值" || bad "sys 磁盘异常"
printf '%s\n' "$SYS_ZH" | grep -Eq '已运行' && ok "sys 开机时长" || bad "sys 开机时长异常"
printf '%s\n' "$SYS_ZH" | grep -Eq '网络 ↓' && ok "sys 网络速率" || bad "sys 网络异常"
SYS_EN="$($BIN --lang en sys 2>&1)"
printf '%s\n' "$SYS_EN" | grep -q 'System' && ok "sys 英文标题" || bad "sys 英文标题异常: $SYS_EN"

echo "== 10. 英文输出冒烟 =="
EN_OUT="$(BRIGHTCTL_LANG=en $BIN --lang en 2>&1)"
printf '%s
' "$EN_OUT" | grep -q 'channel=DDC\|channel=DisplayServices' && ok "英文列屏可用" || bad "英文列屏异常: $EN_OUT"
printf '%s
' "$EN_OUT" | grep -q 'brightness=' && ok "英文字段完整" || bad "英文字段缺失"
BRIGHTCTL_LANG=en $BIN --lang en --help 2>&1 | grep -q '^Usage:' && ok "英文 usage" || bad "英文 usage 异常"
$BIN --lang zh --help 2>&1 | grep -q '用法:' && ok "中文 usage" || bad "中文 usage 异常"

restore_all
trap - EXIT
echo
echo "通过 $pass 项，失败 $fail 项"
[ "$fail" -eq 0 ]
