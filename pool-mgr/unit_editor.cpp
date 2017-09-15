#include "unit_editor.hpp"
#include <iostream>
#include "unit.hpp"
#include <glibmm.h>

namespace horizon {

	class PinEditor: public Gtk::Box {
		public:
			PinEditor(BaseObjectType* cobject, const Glib::RefPtr<Gtk::Builder>& x, class Pin *p, UnitEditor *pa);
			static PinEditor* create(class Pin *p, UnitEditor *pa);
			class Pin *pin;
			UnitEditor *parent;

		private :
			Gtk::Entry *name_entry = nullptr;
			Gtk::Entry *names_entry = nullptr;
			Gtk::ComboBoxText *dir_combo = nullptr;
			Gtk::SpinButton *swap_group_spin_button = nullptr;

	};

	PinEditor::PinEditor(BaseObjectType* cobject, const Glib::RefPtr<Gtk::Builder>& x, Pin *p, UnitEditor *pa) :
		Gtk::Box(cobject), pin(p), parent(pa) {
		x->get_widget("pin_name", name_entry);
		x->get_widget("pin_names", names_entry);
		x->get_widget("pin_direction", dir_combo);
		x->get_widget("pin_swap_group", swap_group_spin_button);

		dir_combo->append(std::to_string(static_cast<int>(Pin::Direction::INPUT)), "Input");
		dir_combo->append(std::to_string(static_cast<int>(Pin::Direction::OUTPUT)), "Output");
		dir_combo->append(std::to_string(static_cast<int>(Pin::Direction::BIDIRECTIONAL)), "Bidirectional");
		dir_combo->append(std::to_string(static_cast<int>(Pin::Direction::PASSIVE)), "Passive");
		dir_combo->append(std::to_string(static_cast<int>(Pin::Direction::POWER_INPUT)), "Power Input");
		dir_combo->append(std::to_string(static_cast<int>(Pin::Direction::POWER_OUTPUT)), "Power Output");
		dir_combo->append(std::to_string(static_cast<int>(Pin::Direction::OPEN_COLLECTOR)), "Open Collector");


		name_entry->set_text(pin->primary_name);
		name_entry->signal_changed().connect([this]{
			pin->primary_name = name_entry->get_text();
		});

		{
			std::stringstream s;
			std::copy(pin->names.begin(), pin->names.end(), std::ostream_iterator<std::string>(s, " "));
			names_entry->set_text(s.str());
		}

		auto propagate = [this](std::function<void(PinEditor*)> fn){
			auto lb = dynamic_cast<Gtk::ListBox*>(get_ancestor(GTK_TYPE_LIST_BOX));
			auto this_row = dynamic_cast<Gtk::ListBoxRow*>(get_ancestor(GTK_TYPE_LIST_BOX_ROW));
			auto rows = lb->get_selected_rows();
			if(rows.size()>1 && this_row->is_selected()) {
				for(auto &row: rows) {
					if(auto ed = dynamic_cast<PinEditor*>(row->get_child())) {
						fn(ed);
					}
				}
			}
		};

		dir_combo->set_active_id(std::to_string(static_cast<int>(pin->direction)));
		dir_combo->signal_changed().connect([this, propagate] {
			propagate([this](PinEditor *ed){ed->dir_combo->set_active_id(dir_combo->get_active_id());});
		});
		swap_group_spin_button->set_value(pin->swap_group);
		swap_group_spin_button->signal_value_changed().connect([this, propagate] {
			pin->swap_group = swap_group_spin_button->get_value_as_int();
			propagate([this](PinEditor *ed){ed->swap_group_spin_button->set_value(pin->swap_group);});
		});
	}

	PinEditor* PinEditor::create(Pin *p, UnitEditor *pa) {
		PinEditor* w;
		Glib::RefPtr<Gtk::Builder> x = Gtk::Builder::create();
		x->add_from_resource("/net/carrotIndustries/horizon/pool-mgr/unit_editor.ui");
		x->get_widget_derived("pin_editor", w, p, pa);
		w->reference();
		return w;
	}

	UnitEditor::UnitEditor(BaseObjectType* cobject, const Glib::RefPtr<Gtk::Builder>& x, Unit *u) :
		Gtk::Box(cobject), unit(u) {
		x->get_widget("unit_name", name_entry);
		x->get_widget("unit_manufacturer", manufacturer_entry);
		x->get_widget("unit_pins", pins_listbox);
		x->get_widget("unit_pins_refresh", refresh_button);
		x->get_widget("pin_add", add_button);
		x->get_widget("pin_delete", delete_button);

		name_entry->set_text(unit->name);
		name_entry->signal_changed().connect([this]{
			unit->name = name_entry->get_text();
		});
		manufacturer_entry->set_text(unit->manufacturer);
		manufacturer_entry->signal_changed().connect([this]{
			unit->manufacturer = manufacturer_entry->get_text();
		});


		pins_listbox->set_sort_func([](Gtk::ListBoxRow *a, Gtk::ListBoxRow *b){
			auto na = dynamic_cast<PinEditor*>(a->get_child())->pin->primary_name;
			auto nb = dynamic_cast<PinEditor*>(b->get_child())->pin->primary_name;
			if(na > nb) {
				return 1;
			}
			else if(na < nb) {
				return -1;
			}
			else {
				return 0;
			}
		});

		for(auto &it: unit->pins) {
			auto ed = PinEditor::create(&it.second, this);
			ed->show_all();
			pins_listbox->append(*ed);
			ed->unreference();
		}
		pins_listbox->invalidate_sort();

		refresh_button->signal_clicked().connect([this]{
			pins_listbox->invalidate_sort();
		});

		delete_button->signal_clicked().connect(sigc::mem_fun(this, &UnitEditor::handle_delete));
		add_button->signal_clicked().connect(sigc::mem_fun(this, &UnitEditor::handle_add));

		pins_listbox->signal_key_press_event().connect([this](GdkEventKey *ev) {
			if(ev->keyval == GDK_KEY_Delete) {
				handle_delete();
				return true;
			}
			return false;
		});
	}

	void UnitEditor::handle_delete() {
		auto rows = pins_listbox->get_selected_rows();
		std::set<int> indices;
		std::set<UUID> uuids;
		for(auto &row: rows) {
			uuids.insert(dynamic_cast<PinEditor*>(row->get_child())->pin->uuid);
			indices.insert(row->get_index());
		}
		for(auto &row: rows) {
			delete row;
		}
		for(auto it=unit->pins.begin(); it!=unit->pins.end();) {
			if(uuids.find(it->first) != uuids.end()) {
				unit->pins.erase(it++);
			}
			else {
				it++;
			}
		}
		for(auto index: indices) {
			auto row = pins_listbox->get_row_at_index(index);
			if(row)
				pins_listbox->select_row(*row);
		}
	}

	static std::string inc_pin_name(const std::string &s, int inc=1) {
		Glib::ustring u(s);
		Glib::MatchInfo ma;
		const auto regex = Glib::Regex::create("^(\\D*)(\\d+)(\\D*)\\d*$");
		if(regex->match(u, ma)) {
			auto number_str = ma.fetch(2);
			auto number = std::stoi(number_str)+inc;
			std::stringstream ss;
			ss << ma.fetch(1);
			ss << std::setfill('0') << std::setw(number_str.size()) << number;
			ss << ma.fetch(3);
			return ss.str();
		}
		else {
			return s;
		}
	}

	void UnitEditor::handle_add() {
		const Pin *pin_selected = nullptr;
		auto rows = pins_listbox->get_selected_rows();
		for(auto &row: rows) {
			pin_selected = dynamic_cast<PinEditor*>(row->get_child())->pin;
		}

		auto uu = UUID::random();
		auto pin = &unit->pins.emplace(uu, uu).first->second;
		if(pin_selected) {
			pin->swap_group = pin_selected->swap_group;
			pin->direction = pin_selected->direction;
			pin->primary_name = inc_pin_name(pin_selected->primary_name);
			pin->names = pin_selected->names;
		}


		auto ed = PinEditor::create(pin, this);
		ed->show_all();
		pins_listbox->append(*ed);
		ed->unreference();

		auto children = pins_listbox->get_children();
		for(auto &ch: children) {
			auto row = dynamic_cast<Gtk::ListBoxRow*>(ch);
			auto ed = dynamic_cast<PinEditor*>(row->get_child());
			if(ed->pin->uuid == uu) {
				pins_listbox->unselect_all();
				pins_listbox->select_row(*row);
				break;
			}
		}

		pins_listbox->invalidate_sort();
	}

	UnitEditor* UnitEditor::create(Unit *u) {
		UnitEditor* w;
		Glib::RefPtr<Gtk::Builder> x = Gtk::Builder::create();
		x->add_from_resource("/net/carrotIndustries/horizon/pool-mgr/unit_editor.ui");
		x->get_widget_derived("unit_editor", w, u);
		w->reference();
		return w;
	}
}