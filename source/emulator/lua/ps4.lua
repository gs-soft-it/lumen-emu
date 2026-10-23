
local cfg = {
	ScreenWidth = 1920;
	ScreenHeight = 1080;
	Neo = true;
	VulkanValidationEnabled = true;
	ShaderValidationEnabled = true;		
	ShaderOptimizationType = 'Performance'; -- None, Size, Performance
	ShaderLogDirection = 'File'; -- Silent, Console, File
	ShaderLogFolder = '_Shaders';
	CommandBufferDumpEnabled = true;
	CommandBufferDumpFolder = '_Buffers';
	PrintfDirection = 'Console'; -- Silent, Console, File
	PrintfOutputFile = '_lumen.txt';
	ProfilerDirection = 'None'; -- None, File, Network,	FileAndNetwork
	ProfilerOutputFile = '_profile.prof';
}

lumen_init(cfg);

lumen_mount('z:/dev/ps4/tests/01_Hello_8/', '/app0');

lumen_load_elf('/app0/main.elf');
lumen_load_elf('/app0/sce_module/libc.prx', 0);
lumen_load_elf('/app0/sce_module/libSceFios2.prx', 0);

lumen_load_symbols('libAudio_1');
lumen_load_symbols('libc_internal_1');
lumen_load_symbols('libDebug_1');
lumen_load_symbols('libDialog_1');
lumen_load_symbols('libDiscMap_1');
lumen_load_symbols('libGraphicsDriver_1');
lumen_load_symbols('libkernel_1');
lumen_load_symbols('libNet_1');
lumen_load_symbols('libPad_1');
lumen_load_symbols('libPlayGo_1');
lumen_load_symbols('libSysmodule_1');
lumen_load_symbols('libSystemService_1');
lumen_load_symbols('libUserService_1');
lumen_load_symbols('libVideoOut_1');

lumen_execute();

