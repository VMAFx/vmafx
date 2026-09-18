-- cordanaLLM/praetor Neovim LSP and Tool Configuration
local lspconfig = require("lspconfig")
local configs = require("lspconfig.configs")

if not configs.standards_lsp then
  configs.standards_lsp = {
    default_config = {
      cmd = { "./bin/standards-lsp" },
      filetypes = { "c", "cpp", "cuda", "go", "python" },
      root_dir = function(fname)
        return lspconfig.util.root_pattern(".standards.yaml", "meson.build", "go.mod", ".git")(fname)
      end,
      settings = {
        standards = {
          hissEnforcement = true,
          maxLOC = 75,
          maxStatements = 50,
        },
      },
    },
  }
end

lspconfig.standards_lsp.setup({})

vim.api.nvim_create_user_command("StandardsAudit", function()
  vim.cmd("!standardsctl audit")
end, { desc = "Audit repository against declared HISS invariants" })

vim.api.nvim_create_user_command("StandardsCompileContext", function()
  vim.cmd("!standardsctl compile-context")
end, { desc = "Compile AGENTS.md cross-agent contexts" })

vim.api.nvim_create_user_command("StandardsVerifyAll", function()
  vim.cmd("!make verify-all")
end, { desc = "Run full standards verification pipeline" })
