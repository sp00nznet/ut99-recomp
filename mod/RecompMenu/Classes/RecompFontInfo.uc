// HUD fonts that keep growing past 1024 pixels wide. Botpack's FontInfo stops
// there, so at 1080p and up the HUD's text shrinks against the screen. Each
// size here grows with the width, from UT's own LadderFonts (8 to 30 point;
// 30 is the ceiling, so very high resolutions still get 30).
// Chosen by [Botpack.ChallengeHUD] FontInfoClass=RecompMenu.RecompFontInfo.
class RecompFontInfo expands FontInfo;

// The largest LadderFont no bigger than Base points scaled by Width / 1280.
static function font Pick(int Base, float Width)
{
	local int Want;

	Want = Base * FMax(1.0, Width / 1280.0);
	if (Want >= 30) return Font(DynamicLoadObject("LadderFonts.UTLadder30", class'Font'));
	if (Want >= 24) return Font(DynamicLoadObject("LadderFonts.UTLadder24", class'Font'));
	if (Want >= 22) return Font(DynamicLoadObject("LadderFonts.UTLadder22", class'Font'));
	if (Want >= 20) return Font(DynamicLoadObject("LadderFonts.UTLadder20", class'Font'));
	if (Want >= 18) return Font(DynamicLoadObject("LadderFonts.UTLadder18", class'Font'));
	if (Want >= 16) return Font(DynamicLoadObject("LadderFonts.UTLadder16", class'Font'));
	if (Want >= 14) return Font(DynamicLoadObject("LadderFonts.UTLadder14", class'Font'));
	if (Want >= 12) return Font(DynamicLoadObject("LadderFonts.UTLadder12", class'Font'));
	if (Want >= 10) return Font(DynamicLoadObject("LadderFonts.UTLadder10", class'Font'));
	return Font(DynamicLoadObject("LadderFonts.UTLadder8", class'Font'));
}

// Below 1024 wide, the stock choices; above, the stock size at 1024 scaled.
static function font GetStaticHugeFont(float Width)
{
	if (Width < 1024) return class'FontInfo'.static.GetStaticHugeFont(Width);
	return Pick(30, Width);
}

static function font GetStaticBigFont(float Width)
{
	if (Width < 1024) return class'FontInfo'.static.GetStaticBigFont(Width);
	return Pick(22, Width);
}

static function font GetStaticMediumFont(float Width)
{
	if (Width < 1024) return class'FontInfo'.static.GetStaticMediumFont(Width);
	return Pick(22, Width);
}

static function font GetStaticSmallFont(float Width)
{
	if (Width < 1024) return class'FontInfo'.static.GetStaticSmallFont(Width);
	return Pick(16, Width);
}

static function font GetStaticSmallestFont(float Width)
{
	if (Width < 1024) return class'FontInfo'.static.GetStaticSmallestFont(Width);
	return Pick(14, Width);
}

static function font GetStaticAReallySmallFont(float Width)
{
	if (Width < 1024) return class'FontInfo'.static.GetStaticAReallySmallFont(Width);
	return Pick(10, Width);
}

static function font GetStaticACompletelyUnreadableFont(float Width)
{
	if (Width < 1024) return class'FontInfo'.static.GetStaticACompletelyUnreadableFont(Width);
	return Pick(8, Width);
}
