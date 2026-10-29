import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from ra4_nano_heap_diagnostic import summarize_free_list

base = 0x20003000
end = base + 4096
memory = {base: 900, base + 4: base + 1024,
          base + 1024: 800, base + 1028: base + 2048,
          base + 2048: 804, base + 2052: 0}
result = summarize_free_list(memory.__getitem__, base, base, end, end + 244)
assert result["total_free_chunk_bytes"] == 2504
assert result["largest_free_chunk_bytes"] == 900 < 1044
assert result["unextended_bytes"] == 244
assert summarize_free_list(memory.__getitem__, 0, base, end, end)["free_chunks"] == 0
for head, modifications in [(base - 4, {}), (base + 1, {}),
                            (base, {base: 4097}), (base, {base: 8192}),
                            (base, {base + 4: base})]:
    altered = dict(memory)
    altered.update(modifications)
    reads = []
    def read(address):
        assert base <= address <= end - 4
        reads.append(address)
        return altered[address]
    try:
        summarize_free_list(read, head, base, end, end)
    except ValueError:
        pass
    else:
        raise AssertionError("malformed list accepted")
    assert len(reads) <= 2
print("RA4_NANO_HEAP_DIAGNOSTIC_PASS")
