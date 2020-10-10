/***************************************************************************
 *   Copyright (C) 2020 by Robert Middleton                                *
 *   robert.middleton@rm5248.com                                           *
 *                                                                         *
 *   This file is part of the dbus-cxx library.                            *
 *                                                                         *
 *   The dbus-cxx library is free software; you can redistribute it and/or *
 *   modify it under the terms of the GNU General Public License           *
 *   version 3 as published by the Free Software Foundation.               *
 *                                                                         *
 *   The dbus-cxx library is distributed in the hope that it will be       *
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty   *
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU   *
 *   General Public License for more details.                              *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this software. If not see <http://www.gnu.org/licenses/>.  *
 ***************************************************************************/
#include <dbus-cxx/enums.h>
#include <dbus-cxx/dbus-cxx-config.h>
#include <dbus-cxx/variant.h>
#include <sigc++/sigc++.h>
#include <memory>

namespace DBus {

/**
 * Base type of Property to allow for storage in e.g. a vector.
 */
class PropertyBase {
protected:
    PropertyBase( std::string interface_name, std::string name, PropertyUpdateType update );

public:

    /**
     * Get the name of this propery.
     * @return
     */
    std::string name() const;

    /**
     * Get the interface that this property is on.
     * @return
     */
    std::string interface_name() const;

    /**
     * Get the value of this property as a Variant.
     *
     * @return
     */
    Variant variant_value() const;

    PropertyUpdateType update_type() const;

    /**
     * This signal is emitted whenever the property changes
     * @return
     */
    sigc::signal<void(Variant)> signal_generic_property_changed();

    /**
     * Set the value of this property.
     *
     * When used on a remote property(a proxy), this will attempt to set
     * the value on the remote object.  If the property is READONLY, this
     * acts as a No-op.
     *
     * When used on a local property(adapter), this will emit the PropertyChanged
     * DBus signal in order to notify clients that the property has updated.
     * Note that the exact value of the PropertyUpdateType will determine what is
     * emitted(invalidated, new value, or invalidation)
     *
     * @param value The new value to set
     */
    void set_value( Variant value );

private:
    class priv_data;

    DBUS_CXX_PROPAGATE_CONST( std::unique_ptr<priv_data> ) m_priv;
};

/**
 * Represents a DBus property.
 *
 * Properties can be Read, Write, or Readonly.  When they change, a signal
 * may be emitted on the bus to listeners.
 */
template <typename T_type>
class Property : public PropertyBase {
private:
    Property( std::string interface_name, std::string name, PropertyUpdateType update ) :
        PropertyBase( interface_name, name, update ) {}

public:
    static Property<T_type> create( std::string interface_name, std::string name, PropertyUpdateType update ) {
        return std::shared_ptr( new Property<T_type>( interface_name, name, update ) );
    }

    sigc::signal<void(T_type)> signal_property_changed() {
        return m_slot;
    }

    void set_value( T_type t ) {
        set_value( Variant( t ) );
    }

    T_type value() const {
        T_type t = variant_value();
        return t;
    }

private:
    sigc::slot<void(T_type)> m_slot;
};

} /* namespace DBus */
