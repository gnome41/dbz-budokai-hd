import json, re, bisect, subprocess
MERGED = r"E:\Games\RecompLauncher\ps3recomp\merged_funcs.json"
out = subprocess.run(["git", "show", "master:recompiled/ppu_recomp.cpp"],
                     cwd=r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd",
                     capture_output=True)
text = out.stdout.decode("utf-8", "replace")
funcs = json.load(open(MERGED))
starts = {int(str(e["start"]),0): int(str(e["end"]),0) for e in funcs}
addrs = set()
for m in re.finditer(r"^\s*(?:static )?void func_([0-9A-Fa-f]{5,8})\(ppu_context", text, re.M):
    a = int(m.group(1), 16)
    if 0x10000 <= a < 0x155000:
        addrs.add(a)
new = sorted(a for a in addrs if a not in starts)
allst = sorted(set(starts) | addrs)
def ns(a):
    i = bisect.bisect_right(allst, a)
    return allst[i] if i < len(allst) else a + 0x40
for a in new:
    starts[a] = ns(a)
json.dump([{"start":"0x%X"%s,"end":"0x%X"%starts[s]} for s in sorted(starts)], open(MERGED,"w"))
print("master text funcs:", len(addrs), "newly added:", len(new), "total:", len(starts))
