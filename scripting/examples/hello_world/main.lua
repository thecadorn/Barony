-- The simplest possible script mod: log a greeting when the game starts.
-- Install by copying this folder to ~/.barony/scripts/hello_world/

barony.onInit(function()
	barony.log("hello world")
end)
