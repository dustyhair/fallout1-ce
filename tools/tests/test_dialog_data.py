import unittest

from dialog_data import _call_arguments, _floating_roles, _split_arguments, _static_message_ids


class DialogDataTest(unittest.TestCase):
    def test_nested_call_arguments(self):
        source = "float_msg(self_obj, message_str(12, random(100, 102)), 2);"
        arguments = next(_call_arguments(source, "float_msg"))
        self.assertEqual(
            _split_arguments(arguments),
            ["self_obj", "message_str(12, random(100, 102))", "2"],
        )

    def test_static_message_ids(self):
        self.assertEqual(_static_message_ids("105"), {105})
        self.assertEqual(_static_message_ids("random(4, 2)"), {2, 3, 4})
        self.assertEqual(_static_message_ids("200 + random(0, 2)"), {200, 201, 202})
        self.assertIsNone(_static_message_ids("local_var(3)"))

    def test_self_floating_lines_use_calling_script_voice(self):
        references = _floating_roles(
            "float_msg(self_obj, message_str(669, random(100, 101)), 0);",
            923,
            False,
            {669: {100, 101}},
        )
        self.assertEqual(
            {(entry.speaker_list_id, entry.message_list_id, entry.message_id) for entry in references},
            {(923, 669, 100), (923, 669, 101)},
        )
        self.assertTrue(all(entry.roles == {"npc", "floating"} for entry in references))

    def test_player_and_indirect_floating_lines(self):
        source = """
        line := message_str(40, 233);
        float_msg(dude_obj, line, 3);
        """
        references = _floating_roles(source, 39, False, {40: {233}})
        self.assertEqual(len(references), 1)
        self.assertEqual(references[0].roles, {"player", "floating"})
        self.assertEqual(references[0].message_list_id, 40)

    def test_player_script_self_is_player_voice(self):
        references = _floating_roles(
            "float_msg(self_obj, message_str(1, 500), 0);",
            1,
            True,
            {1: {500}},
        )
        self.assertEqual(references[0].roles, {"player", "floating"})

    def test_dynamic_message_id_includes_available_list(self):
        references = _floating_roles(
            "float_msg(self_obj, message_str(603, response), 2);",
            603,
            False,
            {603: {101, 102, 103}},
        )
        self.assertEqual({entry.message_id for entry in references}, {101, 102, 103})


if __name__ == "__main__":
    unittest.main()
