// The settings window's frame; RecompGraphicsClient holds the controls.
class RecompGraphicsWindow expands UWindowFramedWindow;

function Created()
{
	bStatusBar = False;
	bSizable = False;
	Super.Created();
	SetSize(320, 230);
	WinLeft = int(Root.WinWidth / 2 - WinWidth / 2);
	WinTop = int(Root.WinHeight / 2 - WinHeight / 2);
}

defaultproperties
{
	ClientClass=Class'RecompGraphicsClient'
	WindowTitle="Recomp Graphics"
}
