// The Mod menu's "Recomp Graphics": opens the settings window. UT lists it
// from RecompMenu.int (MetaClass=UMenu.UMenuModMenuItem).
class RecompMenuItem expands UMenuModMenuItem;

function Execute()
{
	MenuItem.Owner.Root.CreateWindow(class'RecompGraphicsWindow', 100, 100, 320, 230, MenuItem.Owner, True);
}
