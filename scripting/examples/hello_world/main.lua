-- The simplest possible script mod: log a greeting when it loads.
-- Install by copying this folder to ~/.barony/scripts/hello_world/

barony.onInit(function()
	barony.log("hello world")
end)
