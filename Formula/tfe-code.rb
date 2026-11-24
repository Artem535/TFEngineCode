class TfeCode < Formula
  desc "Role-tree code editor and structural renderer for AI coding agents"
  homepage "https://github.com/Artem535/TFEngineCode"
  version "0.6.0"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/Artem535/TFEngineCode/releases/download/v0.6.0/tfe-code-aarch64-apple-darwin.tar.gz"
      sha256 "4bb7f164d791bb5c57fdd9fbe449a160ce8addd59c6ada7ac510be08e7ad58a1"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/Artem535/TFEngineCode/releases/download/v0.6.0/tfe-code-aarch64-unknown-linux-gnu.tar.gz"
      sha256 "9a3479742be93cffd056594b4eebf930bee4490157c5534725385f4a504d82e3"
    end
    on_intel do
      url "https://github.com/Artem535/TFEngineCode/releases/download/v0.6.0/tfe-code-x86_64-unknown-linux-gnu.tar.gz"
      sha256 "8588e2919bfbdead9b3d17d02dfebebe2fc89bdc488e927fa3613f5885a18aba"
    end
  end

  def install
    libexec.install "tfe-code"
    Dir["libobjectbox*"].each do |lib|
      libexec.install lib
    end
    bin.install_symlink libexec/"tfe-code"
  end

  test do
    assert_match "Usage: tfe-code", shell_output("#{bin}/tfe-code help")
  end
end
