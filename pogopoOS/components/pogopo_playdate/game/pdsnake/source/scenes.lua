scenes = {
}

import "scenes/gameplay"
import "scenes/gameover"
import "scenes/main_menu"
import "scenes/more"
import "scenes/stats"

function scenes.update()
	scenes.current.update()
end

function scenes.switchTo(targetScene)
	if targetScene == nil then
		print("ERROR: nil scene attempted to be switched to")
		return
	end

	if scenes.current and scenes.current.denit then
		scenes.current.denit()
	end

	scenes.current = targetScene

	if scenes.current.init then
		scenes.current.init()
	end
end

