namespace Loader.Forms
{
    partial class ImportServerDialog
    {
        private System.ComponentModel.IContainer components = null;

        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        private void InitializeComponent()
        {
            this.serverNameLabel = new System.Windows.Forms.Label();
            this.serverNameTextBox = new System.Windows.Forms.TextBox();
            this.descriptionLabel = new System.Windows.Forms.Label();
            this.descriptionTextBox = new System.Windows.Forms.TextBox();
            this.hostnameLabel = new System.Windows.Forms.Label();
            this.hostnameTextBox = new System.Windows.Forms.TextBox();
            this.portLabel = new System.Windows.Forms.Label();
            this.portNumericUpDown = new System.Windows.Forms.NumericUpDown();
            this.gameTypeLabel = new System.Windows.Forms.Label();
            this.gameTypeComboBox = new System.Windows.Forms.ComboBox();
            this.publicKeyLabel = new System.Windows.Forms.Label();
            this.publicKeyTextBox = new System.Windows.Forms.TextBox();
            this.loadPublicKeyButton = new System.Windows.Forms.Button();
            this.importButton = new System.Windows.Forms.Button();
            this.cancelButton = new System.Windows.Forms.Button();
            ((System.ComponentModel.ISupportInitialize)(this.portNumericUpDown)).BeginInit();
            this.SuspendLayout();
            // 
            // serverNameLabel
            // 
            this.serverNameLabel.AutoSize = true;
            this.serverNameLabel.Location = new System.Drawing.Point(12, 15);
            this.serverNameLabel.Name = "serverNameLabel";
            this.serverNameLabel.Size = new System.Drawing.Size(74, 15);
            this.serverNameLabel.TabIndex = 0;
            this.serverNameLabel.Text = "Server Name";
            // 
            // serverNameTextBox
            // 
            this.serverNameTextBox.Location = new System.Drawing.Point(12, 33);
            this.serverNameTextBox.MaxLength = 128;
            this.serverNameTextBox.Name = "serverNameTextBox";
            this.serverNameTextBox.Size = new System.Drawing.Size(516, 23);
            this.serverNameTextBox.TabIndex = 1;
            // 
            // descriptionLabel
            // 
            this.descriptionLabel.AutoSize = true;
            this.descriptionLabel.Location = new System.Drawing.Point(12, 68);
            this.descriptionLabel.Name = "descriptionLabel";
            this.descriptionLabel.Size = new System.Drawing.Size(67, 15);
            this.descriptionLabel.TabIndex = 2;
            this.descriptionLabel.Text = "Description";
            // 
            // descriptionTextBox
            // 
            this.descriptionTextBox.Location = new System.Drawing.Point(12, 86);
            this.descriptionTextBox.MaxLength = 512;
            this.descriptionTextBox.Name = "descriptionTextBox";
            this.descriptionTextBox.Size = new System.Drawing.Size(516, 23);
            this.descriptionTextBox.TabIndex = 3;
            // 
            // hostnameLabel
            // 
            this.hostnameLabel.AutoSize = true;
            this.hostnameLabel.Location = new System.Drawing.Point(12, 121);
            this.hostnameLabel.Name = "hostnameLabel";
            this.hostnameLabel.Size = new System.Drawing.Size(74, 15);
            this.hostnameLabel.TabIndex = 4;
            this.hostnameLabel.Text = "Hostname/IP";
            // 
            // hostnameTextBox
            // 
            this.hostnameTextBox.Location = new System.Drawing.Point(12, 139);
            this.hostnameTextBox.MaxLength = 256;
            this.hostnameTextBox.Name = "hostnameTextBox";
            this.hostnameTextBox.Size = new System.Drawing.Size(334, 23);
            this.hostnameTextBox.TabIndex = 5;
            // 
            // portLabel
            // 
            this.portLabel.AutoSize = true;
            this.portLabel.Location = new System.Drawing.Point(363, 121);
            this.portLabel.Name = "portLabel";
            this.portLabel.Size = new System.Drawing.Size(29, 15);
            this.portLabel.TabIndex = 6;
            this.portLabel.Text = "Port";
            // 
            // portNumericUpDown
            // 
            this.portNumericUpDown.Location = new System.Drawing.Point(363, 139);
            this.portNumericUpDown.Maximum = new decimal(new int[] {
            65535,
            0,
            0,
            0});
            this.portNumericUpDown.Minimum = new decimal(new int[] {
            1,
            0,
            0,
            0});
            this.portNumericUpDown.Name = "portNumericUpDown";
            this.portNumericUpDown.Size = new System.Drawing.Size(76, 23);
            this.portNumericUpDown.TabIndex = 7;
            this.portNumericUpDown.Value = new decimal(new int[] {
            50050,
            0,
            0,
            0});
            // 
            // gameTypeLabel
            // 
            this.gameTypeLabel.AutoSize = true;
            this.gameTypeLabel.Location = new System.Drawing.Point(454, 121);
            this.gameTypeLabel.Name = "gameTypeLabel";
            this.gameTypeLabel.Size = new System.Drawing.Size(65, 15);
            this.gameTypeLabel.TabIndex = 8;
            this.gameTypeLabel.Text = "Game Type";
            // 
            // gameTypeComboBox
            // 
            this.gameTypeComboBox.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.gameTypeComboBox.FormattingEnabled = true;
            this.gameTypeComboBox.Location = new System.Drawing.Point(454, 139);
            this.gameTypeComboBox.Name = "gameTypeComboBox";
            this.gameTypeComboBox.Size = new System.Drawing.Size(74, 23);
            this.gameTypeComboBox.TabIndex = 9;
            // 
            // publicKeyLabel
            // 
            this.publicKeyLabel.AutoSize = true;
            this.publicKeyLabel.Location = new System.Drawing.Point(12, 176);
            this.publicKeyLabel.Name = "publicKeyLabel";
            this.publicKeyLabel.Size = new System.Drawing.Size(61, 15);
            this.publicKeyLabel.TabIndex = 10;
            this.publicKeyLabel.Text = "Public Key";
            // 
            // publicKeyTextBox
            // 
            this.publicKeyTextBox.AcceptsReturn = true;
            this.publicKeyTextBox.AcceptsTab = true;
            this.publicKeyTextBox.Font = new System.Drawing.Font("Consolas", 9F, System.Drawing.FontStyle.Regular, System.Drawing.GraphicsUnit.Point);
            this.publicKeyTextBox.Location = new System.Drawing.Point(12, 194);
            this.publicKeyTextBox.Multiline = true;
            this.publicKeyTextBox.Name = "publicKeyTextBox";
            this.publicKeyTextBox.ScrollBars = System.Windows.Forms.ScrollBars.Both;
            this.publicKeyTextBox.Size = new System.Drawing.Size(516, 190);
            this.publicKeyTextBox.TabIndex = 11;
            this.publicKeyTextBox.WordWrap = false;
            // 
            // loadPublicKeyButton
            // 
            this.loadPublicKeyButton.Location = new System.Drawing.Point(12, 399);
            this.loadPublicKeyButton.Name = "loadPublicKeyButton";
            this.loadPublicKeyButton.Size = new System.Drawing.Size(124, 28);
            this.loadPublicKeyButton.TabIndex = 12;
            this.loadPublicKeyButton.Text = "Load public.key";
            this.loadPublicKeyButton.UseVisualStyleBackColor = true;
            this.loadPublicKeyButton.Click += new System.EventHandler(this.OnLoadPublicKey);
            // 
            // importButton
            // 
            this.importButton.Location = new System.Drawing.Point(363, 399);
            this.importButton.Name = "importButton";
            this.importButton.Size = new System.Drawing.Size(79, 28);
            this.importButton.TabIndex = 13;
            this.importButton.Text = "Import";
            this.importButton.UseVisualStyleBackColor = true;
            this.importButton.Click += new System.EventHandler(this.OnImport);
            // 
            // cancelButton
            // 
            this.cancelButton.DialogResult = System.Windows.Forms.DialogResult.Cancel;
            this.cancelButton.Location = new System.Drawing.Point(449, 399);
            this.cancelButton.Name = "cancelButton";
            this.cancelButton.Size = new System.Drawing.Size(79, 28);
            this.cancelButton.TabIndex = 14;
            this.cancelButton.Text = "Cancel";
            this.cancelButton.UseVisualStyleBackColor = true;
            // 
            // ImportServerDialog
            // 
            this.AcceptButton = this.importButton;
            this.AutoScaleDimensions = new System.Drawing.SizeF(7F, 15F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.CancelButton = this.cancelButton;
            this.ClientSize = new System.Drawing.Size(540, 441);
            this.Controls.Add(this.cancelButton);
            this.Controls.Add(this.importButton);
            this.Controls.Add(this.loadPublicKeyButton);
            this.Controls.Add(this.publicKeyTextBox);
            this.Controls.Add(this.publicKeyLabel);
            this.Controls.Add(this.gameTypeComboBox);
            this.Controls.Add(this.gameTypeLabel);
            this.Controls.Add(this.portNumericUpDown);
            this.Controls.Add(this.portLabel);
            this.Controls.Add(this.hostnameTextBox);
            this.Controls.Add(this.hostnameLabel);
            this.Controls.Add(this.descriptionTextBox);
            this.Controls.Add(this.descriptionLabel);
            this.Controls.Add(this.serverNameTextBox);
            this.Controls.Add(this.serverNameLabel);
            this.FormBorderStyle = System.Windows.Forms.FormBorderStyle.FixedDialog;
            this.MaximizeBox = false;
            this.MinimizeBox = false;
            this.Name = "ImportServerDialog";
            this.ShowIcon = false;
            this.ShowInTaskbar = false;
            this.StartPosition = System.Windows.Forms.FormStartPosition.CenterParent;
            this.Text = "Import Server";
            ((System.ComponentModel.ISupportInitialize)(this.portNumericUpDown)).EndInit();
            this.ResumeLayout(false);
            this.PerformLayout();

        }

        #endregion

        private System.Windows.Forms.Label serverNameLabel;
        private System.Windows.Forms.TextBox serverNameTextBox;
        private System.Windows.Forms.Label descriptionLabel;
        private System.Windows.Forms.TextBox descriptionTextBox;
        private System.Windows.Forms.Label hostnameLabel;
        private System.Windows.Forms.TextBox hostnameTextBox;
        private System.Windows.Forms.Label portLabel;
        private System.Windows.Forms.NumericUpDown portNumericUpDown;
        private System.Windows.Forms.Label gameTypeLabel;
        private System.Windows.Forms.ComboBox gameTypeComboBox;
        private System.Windows.Forms.Label publicKeyLabel;
        private System.Windows.Forms.TextBox publicKeyTextBox;
        private System.Windows.Forms.Button loadPublicKeyButton;
        private System.Windows.Forms.Button importButton;
        private System.Windows.Forms.Button cancelButton;
    }
}
