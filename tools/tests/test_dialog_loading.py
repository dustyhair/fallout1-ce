import tempfile
import unittest
from pathlib import Path

from dialog_data import load_dialogue


class DialogLoadingTest(unittest.TestCase):
    def test_duplicate_script_names_keep_each_list_id(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            messages = root / "messages"
            scripts = root / "scripts"
            messages.mkdir()
            scripts.mkdir()
            (messages / "SHARED.MSG").write_text("{100}{}{Hello.}\n", encoding="cp1252")
            (root / "SCRIPTS.LST").write_text(
                "SHARED.INT ; First\nSHARED.INT ; Second\n",
                encoding="cp1252",
            )
            (scripts / "SHARED.ssl").write_text(
                "float_msg(self_obj, message_str(1, 100), 0);\n"
                "float_msg(self_obj, message_str(2, 100), 0);\n",
                encoding="cp1252",
            )

            records = load_dialogue(messages, None, root / "SCRIPTS.LST", scripts)

        self.assertEqual({record.message_list_id for record in records}, {1, 2})

    def test_empty_decompiled_script_renders_both_conversation_roles(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            messages = root / "messages"
            scripts = root / "scripts"
            messages.mkdir()
            scripts.mkdir()
            (messages / "TANDI.MSG").write_text(
                "{113}{}{It's okay. Can I ask you a few questions, though?}\n",
                encoding="cp1252",
            )
            (root / "SCRIPTS.LST").write_text("TANDI.INT ; Tandi\n", encoding="cp1252")
            (scripts / "TANDI.ssl").write_text("", encoding="cp1252")

            records = load_dialogue(messages, None, root / "SCRIPTS.LST", scripts)

        self.assertEqual(len(records), 1)
        self.assertEqual(records[0].roles, {"npc", "player", "unclassified"})


if __name__ == "__main__":
    unittest.main()
