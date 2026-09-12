class MemoryForAi < Formula
  desc "Fast code intelligence engine for AI coding agents"
  homepage "https://github.com/LonelyTraderBay/memory-for-ai"
  version "0.10.8"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v#{version}/memory-for-ai-darwin-arm64.tar.gz"
      sha256 "83ea8b20128b33c4c6cf29e47885bf33d65b46a6e6bdceefdc41cbcf3a313f86"
    end
    on_intel do
      url "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v#{version}/memory-for-ai-darwin-amd64.tar.gz"
      sha256 "136ac91db00e6a50c2c98581145149bed6c54eae6019a34d08bbb14f29465c56"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v#{version}/memory-for-ai-linux-arm64.tar.gz"
      sha256 "55d04fb7b9d440c75f4a14f4b2717a895bd58827cda5729654dd3dbf5daf8f34"
    end
    on_intel do
      url "https://github.com/LonelyTraderBay/memory-for-ai/releases/download/v#{version}/memory-for-ai-linux-amd64.tar.gz"
      sha256 "c7451af0e9e94ba6d02e5644ce94794f5dcb4617e2ce5164b1c6eab65d52d57b"
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
