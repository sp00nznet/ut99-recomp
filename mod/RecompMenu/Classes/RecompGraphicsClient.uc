// The recompilation's graphics settings, in the game's own menus.
//
// Anti-aliasing, texture filtering, the frame cap and vsync are the host's
// (src/runtime/glfx.c): saved here under [RecompMenu.RecompGraphicsClient] in
// the game's ini, read by the host at the next start. HUD fonts and the menu
// size are the game's own settings and change now.
class RecompGraphicsClient expands UWindowDialogClientWindow
	config;

var config int MSAA;     // samples; 0 off
var config int Aniso;    // anisotropy; 0 the renderer's own filtering
var config int FPSCap;   // frames a second; 0 none
var config int VSync;    // -1 the driver's, 0 off, 1 on

var UWindowComboControl MSAACombo, AnisoCombo, CapCombo, VSyncCombo, FontCombo, MenuCombo;
var UWindowLabelControl Note;
var bool bReady;         // set after Created: the combos' first selections are not changes

function UWindowComboControl AddCombo(string Text, string Help, int Top)
{
	local UWindowComboControl C;

	C = UWindowComboControl(CreateControl(class'UWindowComboControl', 10, Top, WinWidth - 20, 1));
	C.SetText(Text);
	C.SetHelpText(Help);
	C.SetFont(F_Normal);
	C.SetEditable(False);
	C.EditBoxWidth = 120;
	return C;
}

function Choose(UWindowComboControl C, string V)
{
	C.SetSelectedIndex(Max(0, C.FindItemIndex2(V)));
}

function Created()
{
	Super.Created();

	MSAACombo = AddCombo("Anti-aliasing", "Smooths polygon edges (multisampling). Next start.", 10);
	MSAACombo.AddItem("Off", "0");
	MSAACombo.AddItem("2x", "2");
	MSAACombo.AddItem("4x", "4");
	MSAACombo.AddItem("8x", "8");
	Choose(MSAACombo, string(MSAA));

	AnisoCombo = AddCombo("Texture filtering", "Trilinear and anisotropic: sharp floors and walls at an angle. Next start.", 35);
	AnisoCombo.AddItem("Original", "0");
	AnisoCombo.AddItem("4x anisotropic", "4");
	AnisoCombo.AddItem("8x anisotropic", "8");
	AnisoCombo.AddItem("16x anisotropic", "16");
	Choose(AnisoCombo, string(Aniso));

	CapCombo = AddCombo("Frame rate cap", "UT's physics go wrong at very high frame rates. Next start.", 60);
	CapCombo.AddItem("None", "0");
	CapCombo.AddItem("60", "60");
	CapCombo.AddItem("120", "120");
	CapCombo.AddItem("144", "144");
	CapCombo.AddItem("200", "200");
	CapCombo.AddItem("240", "240");
	Choose(CapCombo, string(FPSCap));

	VSyncCombo = AddCombo("Vertical sync", "Wait for the display between frames. Next start.", 85);
	VSyncCombo.AddItem("Driver's choice", "-1");
	VSyncCombo.AddItem("Off", "0");
	VSyncCombo.AddItem("On", "1");
	Choose(VSyncCombo, string(VSync));

	FontCombo = AddCombo("HUD fonts", "Scaled: the HUD's text grows with the resolution. Next map.", 110);
	FontCombo.AddItem("Original", "Botpack.FontInfo");
	FontCombo.AddItem("Scaled", "RecompMenu.RecompFontInfo");
	Choose(FontCombo, class'ChallengeHUD'.default.FontInfoClass);

	MenuCombo = AddCombo("Menu size", "Double: the menus and their fonts at twice the size, for high resolutions.", 135);
	MenuCombo.AddItem("Normal", "1");
	MenuCombo.AddItem("Double", "2");
	Choose(MenuCombo, string(int(Root.GUIScale)));

	Note = UWindowLabelControl(CreateControl(class'UWindowLabelControl', 10, 165, WinWidth - 20, 1));
	Note.SetText("Rendering settings apply at the next start.");
	Note.SetFont(F_Normal);

	bReady = True;
}

// Live: the HUD in play gets the new fonts now, and the setting is saved.
function SetHUDFonts(string ClassName)
{
	local ChallengeHUD H;

	H = ChallengeHUD(GetPlayerOwner().myHUD);
	if (H == None)
	{
		class'ChallengeHUD'.default.FontInfoClass = ClassName;
		class'ChallengeHUD'.static.StaticSaveConfig();
		return;
	}
	H.FontInfoClass = ClassName;
	H.SaveConfig();
	if (H.MyFonts != None)
		H.MyFonts.Destroy();
	H.MyFonts = FontInfo(H.Spawn(Class<Actor>(DynamicLoadObject(ClassName, class'Class'))));
}

function Notify(UWindowDialogControl C, byte E)
{
	Super.Notify(C, E);
	if (!bReady || E != DE_Change)
		return;

	if (C == MSAACombo) MSAA = int(MSAACombo.GetValue2());
	else if (C == AnisoCombo) Aniso = int(AnisoCombo.GetValue2());
	else if (C == CapCombo) FPSCap = int(CapCombo.GetValue2());
	else if (C == VSyncCombo) VSync = int(VSyncCombo.GetValue2());
	else if (C == FontCombo) { SetHUDFonts(FontCombo.GetValue2()); return; }
	else if (C == MenuCombo) { Root.SetScale(float(MenuCombo.GetValue2())); Root.SaveConfig(); return; }
	SaveConfig();
}

defaultproperties
{
	VSync=-1
}
