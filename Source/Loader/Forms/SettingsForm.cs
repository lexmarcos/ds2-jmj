using System;
using System.IO;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Loader
{
    public partial class SettingsForm : Form
    {
        public string ExeLocation = "";
        private bool DoNotSaveSettings = false;

        public SettingsForm()
        {
            InitializeComponent();
        }

        private void OnLoad(object sender, EventArgs e)
        {
            DoNotSaveSettings = true;
            UseSeperateSavesCheckbox.Checked = ProgramSettings.Default.use_seperate_saves;
            PatchPhantomTimersCheckbox.Checked = ProgramSettings.Default.ds2_patch_phantom_timers;
            PhantomTimerSecondsInput.Value = ClampToInputRange(ProgramSettings.Default.ds2_phantom_timer_seconds);
            DoNotSaveSettings = false;

            UpdateState();
        }

        private void UpdateState()
        {
            CopySavesButton.Enabled = ProgramSettings.Default.use_seperate_saves;

            PhantomTimerSecondsLabel.Enabled = ProgramSettings.Default.ds2_patch_phantom_timers;
            PhantomTimerSecondsInput.Enabled = ProgramSettings.Default.ds2_patch_phantom_timers;
        }

        // Settings can hold a value from an older build that the input no longer accepts.
        private decimal ClampToInputRange(double Seconds)
        {
            if (double.IsNaN(Seconds) || Seconds <= 0.0)
            {
                return PhantomTimerSecondsInput.Value;
            }

            decimal Value = (decimal)Math.Clamp(
                Seconds,
                (double)PhantomTimerSecondsInput.Minimum,
                (double)PhantomTimerSecondsInput.Maximum);

            return Value;
        }

        private void CopySavesClicked(object sender, EventArgs e)
        {   
            if (MessageBox.Show("This will overwrite any existing DSOS saves that exist, are you sure you wish to do this?", "Warning", MessageBoxButtons.YesNo, MessageBoxIcon.Exclamation) != DialogResult.Yes)
            {
                return;
            }

            int FilesCopied = 0;
            
            string BasePath = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData) + @"\DarkSoulsIII";
            FilesCopied += CopySavesInDirectory(BasePath);
            
            BasePath = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData) + @"\DarkSoulsII";
            FilesCopied += CopySavesInDirectory(BasePath);
            
            MessageBox.Show("Copied " + FilesCopied.ToString() + " retail saves to dsos.");
        }

        private int CopySavesInDirectory(string BasePath)
        {
            int FilesCopied = 0;
            
            if (!Directory.Exists(BasePath))
            {
                return 0;
            }

            string[] RetailFiles = System.IO.Directory.GetFiles(BasePath, "*.sl2", SearchOption.AllDirectories);
            foreach (string file in RetailFiles)
            {
                string NewPath = Path.ChangeExtension(file, ".ds3os");
                Console.WriteLine(file + " -> " + NewPath);

                File.Copy(file, NewPath, true);

                FilesCopied++;
            }
            
            return FilesCopied;
        }

        private void SettingChanged(object sender, EventArgs e)
        {
            if (DoNotSaveSettings)
            {
                return;
            }

            ProgramSettings.Default.use_seperate_saves = UseSeperateSavesCheckbox.Checked;
            ProgramSettings.Default.ds2_patch_phantom_timers = PatchPhantomTimersCheckbox.Checked;
            ProgramSettings.Default.ds2_phantom_timer_seconds = (double)PhantomTimerSecondsInput.Value;
            ProgramSettings.Default.Save();

            UpdateState();
        }
    }
}
