from pathlib import Path
import importlib.util,tempfile
root=Path(__file__).resolve().parents[1]
p=root/'.'
spec=importlib.util.spec_from_file_location('artifacts',p/'tools/runtime_bundle/copy_image_artifacts.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
with tempfile.TemporaryDirectory() as directory:
    d=Path(directory);image=d/'image.bin'
    frozen=p/'tools/partitions/partition_cfg_4M_bridge_v1.toml'
    for size,valid in ((0,False),(1474560,True),(1474561,False),(1769472,False)):
        with image.open('wb') as f:f.truncate(size)
        try:m.validate_image_capacity(image,frozen)
        except RuntimeError:assert not valid
        else:assert valid
    stage=d/'stage.toml';stage.write_text('[[pt_entry]]\nname="FW"\nsize0=0x100\nsize1=0\n')
    image.write_bytes(b'X'*256);m.validate_image_capacity(image,stage)
    stage.write_text(stage.read_text()+stage.read_text())
    try:m.validate_image_capacity(image,stage)
    except RuntimeError:pass
    else:raise AssertionError('duplicate FW accepted')
hook=next(x for x in (p/'platform.txt').read_text().splitlines() if x.startswith('recipe.hooks.objcopy.postobjcopy.2.pattern='))
assert '{build.partition_config}' in hook
print('BL616CL_PLATFORM_POSTPROCESS_BOTH_SLOT_OVERFLOW_GATE_PASS')
