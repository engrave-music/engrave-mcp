-- One environment location shared by host configuration and build callbacks.
ENGRAVE_PIXI_ENV = path.join(os.projectdir(), ".pixi", "envs", "default")
ENGRAVE_PIXI_PYTHON = path.join(ENGRAVE_PIXI_ENV, is_host("windows") and "python.exe" or "bin/python")
