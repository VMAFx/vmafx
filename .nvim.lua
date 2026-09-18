-- Load project-level standards configuration
local status_ok, res = pcall(require, "standards")
if not status_ok then
  -- Fallback inline load if lua path is local
  local config_path = vim.fn.getcwd() .. "/lua/standards.lua"
  if vim.fn.filereadable(config_path) == 1 then
    dofile(config_path)
  end
end
