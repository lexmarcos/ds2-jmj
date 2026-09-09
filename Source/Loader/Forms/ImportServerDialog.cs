using System;
using System.IO;
using System.Windows.Forms;

namespace Loader.Forms
{
    public partial class ImportServerDialog : Form
    {
        public ServerConfig ImportedServer { get; private set; }

        public ImportServerDialog(GameType DefaultGameType)
        {
            InitializeComponent();

            gameTypeComboBox.Items.Add(GameType.DarkSouls2.ToString());
            gameTypeComboBox.Items.Add(GameType.DarkSouls3.ToString());
            gameTypeComboBox.SelectedItem = DefaultGameType.ToString();
        }

        public static string NormalizePublicKey(string PublicKey)
        {
            return PublicKey.Replace("\r\n", "\n").Replace("\r", "\n").Trim() + "\n";
        }

        private void OnLoadPublicKey(object sender, EventArgs e)
        {
            using (OpenFileDialog Dialog = new OpenFileDialog())
            {
                Dialog.Filter = "Public Key Files|*.key;*.pem;*.txt|All Files|*.*";
                Dialog.Title = "Select Server Public Key";

                if (Dialog.ShowDialog(this) == DialogResult.OK)
                {
                    publicKeyTextBox.Text = File.ReadAllText(Dialog.FileName);
                }
            }
        }

        private void OnImport(object sender, EventArgs e)
        {
            string ErrorMessage;
            ServerConfig Config;

            if (!TryBuildServerConfig(out Config, out ErrorMessage))
            {
                MessageBox.Show(ErrorMessage, "Invalid Server", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            ImportedServer = Config;
            DialogResult = DialogResult.OK;
            Close();
        }

        private bool TryBuildServerConfig(out ServerConfig Config, out string ErrorMessage)
        {
            Config = null;
            ErrorMessage = "";

            string Name = serverNameTextBox.Text.Trim();
            string Description = descriptionTextBox.Text.Trim();
            string Hostname = hostnameTextBox.Text.Trim();
            string GameType = gameTypeComboBox.SelectedItem as string;
            string PublicKey = NormalizePublicKey(publicKeyTextBox.Text);

            if (string.IsNullOrWhiteSpace(Name))
            {
                ErrorMessage = "Server name is required.";
                return false;
            }

            if (string.IsNullOrWhiteSpace(Hostname))
            {
                ErrorMessage = "Hostname/IP is required.";
                return false;
            }

            if (string.IsNullOrWhiteSpace(GameType))
            {
                ErrorMessage = "Game type is required.";
                return false;
            }

            if (!PublicKey.Contains("-----BEGIN RSA PUBLIC KEY-----") ||
                !PublicKey.Contains("-----END RSA PUBLIC KEY-----"))
            {
                ErrorMessage = "Public key must be an RSA public key.";
                return false;
            }

            Config = new ServerConfig
            {
                Id = Guid.NewGuid().ToString(),
                Name = Name,
                Description = Description,
                Port = (int)portNumericUpDown.Value,
                Hostname = Hostname,
                PrivateHostname = Hostname,
                PublicKey = PublicKey,
                ManualImport = true,
                IpAddress = Hostname,
                PlayerCount = 0,
                PasswordRequired = false,
                ModsWhiteList = "",
                ModsBlackList = "",
                ModsRequiredList = "",
                AllowSharding = false,
                WebAddress = "",
                IsShard = false,
                GameType = GameType
            };

            return true;
        }
    }
}
