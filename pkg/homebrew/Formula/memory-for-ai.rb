class MemoryForAi < Formula
  desc "Fast code intelligence engine for AI coding agents"
  homepage "https://github.com/LonelyTraderBay/memory-for-ai"
  version "0.10.9"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v#{version}/memory-for-ai-darwin-arm64.tar.gz"
      sha256 "299c3bdcdca53317db6b2a60a83da5683e726fdbce5c83371da6e8f62b01010b"
    end
    on_intel do
      url "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v#{version}/memory-for-ai-darwin-amd64.tar.gz"
      sha256 "8304613bc8b7f5878ee1fab42c7e9fdf90a9c3b0f288942b1acb398b910db095"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v#{version}/memory-for-ai-linux-arm64.tar.gz"
      sha256 "9cf9a5e356e5a84a109152a58a7b0b64f96468e82706d88b6ed9b46654b4c3e3"
    end
    on_intel do
      url "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v#{version}/memory-for-ai-linux-amd64.tar.gz"
      sha256 "33b7e30849fc4f481797468e78b07f48b481eba9072b2ad3b1cb957e55d09f0c"
    end
  end

  def install
    bin.install "memory-for-ai"
    # Third-party attribution bundle (present in archives since v0.8.1)
    doc.install "THIRD_PARTY_NOTICES.md" if File.exist?("THIRD_PARTY_NOTICES.md")
  end

  def caveats
    <<~EOS
      Run the following to configure your coding agents:
        memory-for-ai install

      To tap this formula directly:
        brew tap lonelytraderbay/memory-for-ai https://github.com/LonelyTraderBay/memory-for-ai
        brew install memory-for-ai
    EOS
  end

  test do
    assert_match "memory-for-ai", shell_output("#{bin}/memory-for-ai --version")
  end
end
