/*##############################################################################

    Copyright (C) 2011 HPCC Systems.

    All rights reserved. This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
############################################################################## */


namesRecord :=
            RECORD
string20        surname;
string10        forename;
integer2        age := 25;
            END;

namesTable := dataset('x',namesRecord,FLAT);


x := dedup(namesTable, age);

inRecord := record
string20 name;
    end;

includeMatches := true : stored('includeMatches');

outRecord := record(inRecord)
dataset(namesRecord) matches;
    end;

z := dataset('in', inRecord, thor);

outRecord t(inRecord l) := transform
    self := l;
    self.matches := x(includeMatches, surname = l.name, l.name <> '');
    end;

output(project(z, t(LEFT)));


